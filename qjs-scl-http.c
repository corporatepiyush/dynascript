/*
 * scl:http -- native HTTP/1.1 client backed by secure-c-libs.
 *
 *   import { HttpClient } from "scl:http";
 *   const c = new HttpClient();
 *   try {
 *     const r = c.get("http://example.com/");        // { status, statusText,
 *     print(r.status, r.body);                        //   ok, headers, body }
 *     const p = c.post("http://host/api", '{"a":1}',
 *                      { "Content-Type": "application/json" });
 *   } finally {
 *     c.close();                 // deterministic free (arena destroyed)
 *   }
 *
 * Memory model (see qjs-scl.h): every HttpClient owns a private SCL arena; the
 * scl_http_client_t and all of its buffers are allocated from that arena, so a
 * single scl_alloc_arena_destroy() reclaims everything on .close() (the class
 * finalizer is only a safety net for a leaked client). A response's native
 * headers/body are COPIED into JS strings at the call boundary and the native
 * result is freed (scl_http_client_request_free) BEFORE returning -- no arena
 * pointer ever escapes into a JS value.
 *
 * Only HttpClient is exposed. HttpServer is a deliberate follow-up: its handler
 * runs on the server's worker threads, and calling back into a single-threaded
 * JSContext from those threads cannot be made ASan/TSan-clean without a
 * cross-thread marshal to the JS thread. See the note at the end of this file.
 */
#include "qjs-scl.h"

#if defined(CONFIG_SCL_MODULES) && defined(CONFIG_SCL_MODULE_HTTP)

#include "scl_http_client.h"
#include <string.h>
#include <stdio.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- HttpClient: HTTP/1.1 client (scl_http_client) ---------- */

static JSClassID js_scl_http_client_class_id;

static void js_scl_http_client_dispose(void *native, scl_allocator_t *arena)
{
    /* The framework destroys `arena` right after this returns; destroy() here
     * still matters -- it closes the live TCP socket (an OS fd the arena does
     * not own) and scrubs sensitive response bytes. */
    (void)arena;
    scl_http_client_destroy((scl_http_client_t *)native);
}

static const JSClassDef js_scl_http_client_class = {
    "HttpClient",
    .finalizer = js_scl_finalizer,
};

static JSValue js_scl_http_client_ctor(JSContext *ctx, JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    scl_allocator_t *arena;
    scl_http_client_t *client = NULL;
    int64_t max_body = 0; /* 0 => library default (SCL_HTTP_CLIENT_MAX_BODY_BUF) */

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt64(ctx, &max_body, argv[0]))
            return JS_EXCEPTION;
        if (max_body < 0)
            max_body = 0;
    }
    arena = js_scl_arena_new(ctx);
    if (!arena)
        return JS_EXCEPTION;
    if (scl_http_client_init(arena, &client, (size_t)max_body) != SCL_OK ||
        !client) {
        scl_alloc_arena_destroy(arena);
        return JS_ThrowOutOfMemory(ctx);
    }
    return js_scl_resource_wrap(ctx, js_scl_http_client_class_id, arena, client,
                                js_scl_http_client_dispose);
}

/* ---------- helpers: JS <-> native at the boundary ---------- */

/* A header name/value must not smuggle CRLF into the request (header
 * injection). Returns 1 if `s` (length n) is clean. */
static int http_header_token_ok(const char *s, size_t n)
{
    return memchr(s, '\r', n) == NULL && memchr(s, '\n', n) == NULL;
}

/*
 * Build the extra-header block SCL wants ("Name: Value\r\n...") from the JS
 * `headers` argument. Accepts either a pre-formatted string (passed through)
 * or a plain object { name: value, ... }. Returns a js_malloc'd NUL-terminated
 * string the caller frees with js_free(), or NULL. NULL with *perr == 0 means
 * "send no extra headers"; NULL with *perr == 1 means a JS exception is
 * pending. The string is built with the JS allocator (not the arena) so it is
 * fully reclaimed per request and never grows the client's arena.
 */
static char *http_headers_to_string(JSContext *ctx, JSValueConst headers,
                                    int *perr)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    char *buf = NULL;
    size_t cap = 0, used = 0;

    *perr = 0;
    if (JS_IsUndefined(headers) || JS_IsNull(headers))
        return NULL;

    if (JS_IsString(headers)) {
        const char *s = JS_ToCString(ctx, headers);
        char *out;
        size_t n;
        if (!s) {
            *perr = 1;
            return NULL;
        }
        n = strlen(s);
        out = js_malloc(ctx, n + 1);
        if (out)
            memcpy(out, s, n + 1);
        else
            *perr = 1;
        JS_FreeCString(ctx, s);
        return out;
    }

    if (!JS_IsObject(headers))
        return NULL; /* numbers/bools etc: treat as "no extra headers" */

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const char *val;
        JSValue v;
        size_t nl, vl, need;

        if (!name)
            goto fail;
        v = JS_GetProperty(ctx, headers, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        val = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!val) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        nl = strlen(name);
        vl = strlen(val);
        if (!http_header_token_ok(name, nl) || !http_header_token_ok(val, vl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            js_free(ctx, buf);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx, "header name/value must not contain CR or LF");
            return NULL;
        }
        need = nl + 2 + vl + 2; /* "Name" ": " "Value" "\r\n" */
        if (used + need + 1 > cap) {
            size_t nc = cap ? cap * 2 : 128;
            char *nb;
            while (nc < used + need + 1)
                nc *= 2;
            nb = js_realloc(ctx, buf, nc);
            if (!nb) {
                JS_FreeCString(ctx, name);
                JS_FreeCString(ctx, val);
                goto fail;
            }
            buf = nb;
            cap = nc;
        }
        memcpy(buf + used, name, nl);
        used += nl;
        buf[used++] = ':';
        buf[used++] = ' ';
        memcpy(buf + used, val, vl);
        used += vl;
        buf[used++] = '\r';
        buf[used++] = '\n';
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (buf)
        buf[used] = '\0';
    return buf; /* NULL when the object had no own string keys */

 fail:
    js_free(ctx, buf);
    JS_FreePropertyEnum(ctx, tab, len);
    *perr = 1;
    return NULL;
}

/* Parse SCL's flat "Name: Value\0Name: Value\0" header block into a JS object.
 * Names keep the case the server sent; on a duplicate name the last value wins
 * (adequate for common headers -- merging Set-Cookie is a follow-up). */
static JSValue http_headers_object(JSContext *ctx,
                                   const scl_http_client_response_t *resp)
{
    JSValue obj = JS_NewObject(ctx);
    const char *p, *end;

    if (JS_IsException(obj))
        return obj;
    if (!resp->headers || resp->headers_len == 0)
        return obj;

    p = resp->headers;
    end = resp->headers + resp->headers_len;
    while (p < end && *p) {
        const char *q = p;
        const char *colon;
        size_t line_len;
        while (q < end && *q)
            q++;
        line_len = (size_t)(q - p);
        colon = memchr(p, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char *val = colon + 1;
            size_t val_len = line_len - name_len - 1;
            JSAtom key;
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }
            key = JS_NewAtomLen(ctx, p, name_len);
            if (key != JS_ATOM_NULL) {
                JS_DefinePropertyValue(ctx, obj, key,
                                       JS_NewStringLen(ctx, val, val_len),
                                       JS_PROP_C_W_E);
                JS_FreeAtom(ctx, key);
            }
        }
        p = q + 1; /* skip the NUL terminator */
    }
    return obj;
}

/* Copy a completed native response into a fresh JS object. Does NOT free the
 * native response (the caller does, right after). */
static JSValue http_build_response(JSContext *ctx,
                                   const scl_http_client_response_t *resp)
{
    JSValue obj = JS_NewObject(ctx);
    JSValue headers;

    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueStr(ctx, obj, "status",
                              JS_NewInt32(ctx, resp->status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
                              JS_NewString(ctx, resp->status_text),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx, obj, "ok",
        JS_NewBool(ctx, resp->status >= 200 && resp->status < 300),
        JS_PROP_C_W_E);

    headers = http_headers_object(ctx, resp);
    if (JS_IsException(headers)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, obj, "headers", headers, JS_PROP_C_W_E);

    JS_DefinePropertyValueStr(
        ctx, obj, "body",
        JS_NewStringLen(ctx, resp->body ? (const char *)resp->body : "",
                        resp->body_len),
        JS_PROP_C_W_E);
    return obj;
}

/* Turn an SCL error into a thrown JS Error. .message is human-readable; the
 * integer .sclError is the stable machine-branchable code. Returns
 * JS_EXCEPTION. */
static JSValue http_throw_error(JSContext *ctx, scl_error_t err,
                                const char *method, const char *url)
{
    JSValue e;
    char msg[512];

    snprintf(msg, sizeof(msg), "HTTP %s %s failed: %s", method, url,
             scl_error_string(err));
    e = JS_NewError(ctx);
    if (JS_IsException(e))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "sclError", JS_NewInt32(ctx, (int)err),
                              JS_PROP_C_W_E);
    return JS_Throw(ctx, e);
}

/* Shared request driver. `method` is a C string; url/body/headers are JS
 * values (body/headers may be undefined). Everything native is freed before
 * returning, on both the success and error paths. */
static JSValue http_perform(JSContext *ctx, JSValueConst this_val,
                            const char *method, JSValueConst url_val,
                            JSValueConst body_val, JSValueConst headers_val)
{
    JSSclResource *r;
    const char *url = NULL;
    const char *body = NULL; /* borrowed from JS_ToCStringLen */
    size_t body_len = 0;
    char *hdr = NULL; /* js_malloc'd, or NULL for "no extra headers" */
    int hdr_err = 0;
    scl_http_client_response_t resp;
    scl_error_t err;
    JSValue result;

    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");

    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;

    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        body = JS_ToCStringLen(ctx, &body_len, body_val);
        if (!body) {
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }

    hdr = http_headers_to_string(ctx, headers_val, &hdr_err);
    if (hdr_err) {
        if (body)
            JS_FreeCString(ctx, body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    /* Resolve the client AFTER all the JS-invoking coercions above: coercing
     * url/body/headers can run user JS (valueOf/toString/Proxy) that close()s
     * this client; resource_get throws if so, before we touch r->native. */
    r = js_scl_resource_get(ctx, this_val, js_scl_http_client_class_id);
    if (!r) {
        js_free(ctx, hdr);
        if (body)
            JS_FreeCString(ctx, body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    /* Force a fresh TCP connection for every request. The SCL client's
     * keep-alive path (scl_http_client.c) does not reset its read buffer
     * between requests on a reused connection, so the 2nd+ response on a kept
     * connection is mis-framed (its body reads stale buffer bytes). Until that
     * is fixed upstream we disconnect first, making each request behave like
     * the always-correct first request. Re-enabling keep-alive is a follow-up.
     * disconnect() is idempotent and a no-op when not connected. */
    scl_http_client_disconnect((scl_http_client_t *)r->native);

    memset(&resp, 0, sizeof(resp));
    err = scl_http_client_request((scl_http_client_t *)r->native, method, url,
                                  hdr, body, body_len, &resp);
    if (err == SCL_OK)
        result = http_build_response(ctx, &resp);
    else
        result = http_throw_error(ctx, err, method, url);

    /* Free the native result (safe even when partially filled on error) so no
     * native pointer escapes into JS and the arena bytes are reclaimable. */
    scl_http_client_request_free(r->arena, &resp);

    js_free(ctx, hdr);
    if (body)
        JS_FreeCString(ctx, body);
    JS_FreeCString(ctx, url);
    return result;
}

static JSValue js_scl_http_client_get(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    return http_perform(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
                        argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue js_scl_http_client_post(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return http_perform(ctx, this_val, "POST", argv[0],
                        argc > 1 ? argv[1] : JS_UNDEFINED,
                        argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue js_scl_http_client_request(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const char *method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
                                 "request(method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = http_perform(ctx, this_val, method, argv[1],
                       argc > 2 ? argv[2] : JS_UNDEFINED,
                       argc > 3 ? argv[3] : JS_UNDEFINED);
    JS_FreeCString(ctx, method);
    return res;
}

static JSValue js_scl_http_client_set_timeout(JSContext *ctx,
                                              JSValueConst this_val, int argc,
                                              JSValueConst *argv)
{
    JSSclResource *r;
    int64_t ms;

    if (JS_ToInt64(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    r = js_scl_resource_get(ctx, this_val, js_scl_http_client_class_id);
    if (!r)
        return JS_EXCEPTION;
    scl_http_client_set_timeout((scl_http_client_t *)r->native, ms);
    return JS_UNDEFINED;
}

static JSValue js_scl_http_client_disconnect(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv)
{
    JSSclResource *r =
        js_scl_resource_get(ctx, this_val, js_scl_http_client_class_id);

    if (!r)
        return JS_EXCEPTION;
    scl_http_client_disconnect((scl_http_client_t *)r->native);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_scl_http_client_proto[] = {
    JS_CFUNC_DEF("get", 1, js_scl_http_client_get),
    JS_CFUNC_DEF("post", 2, js_scl_http_client_post),
    JS_CFUNC_DEF("request", 2, js_scl_http_client_request),
    JS_CFUNC_DEF("setTimeout", 1, js_scl_http_client_set_timeout),
    JS_CFUNC_DEF("disconnect", 0, js_scl_http_client_disconnect),
};

/* ---------- module registration ---------- */

static int register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                          const JSClassDef *def,
                          const JSCFunctionListEntry *proto_funcs, int n_funcs,
                          JSCFunction *ctor_fn, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    js_scl_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

static int js_scl_http_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (register_class(ctx, m, &js_scl_http_client_class_id,
                       &js_scl_http_client_class, js_scl_http_client_proto,
                       countof(js_scl_http_client_proto),
                       js_scl_http_client_ctor, "HttpClient") < 0)
        return -1;
    return 0;
}

int js_scl_init_http(JSContext *ctx)
{
    JSModuleDef *m =
        JS_NewCModule(ctx, "scl:http", js_scl_http_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "HttpClient");
    return 0;
}

/*
 * HttpServer -- deferred (follow-up), on purpose.
 *
 * scl_http_server spawns an acceptor + N worker threads; the application
 * handler (scl_http_handler_fn) is invoked ON A WORKER THREAD. A QuickJS
 * JSContext/JSRuntime is single-threaded (only SharedArrayBuffer atomics are
 * cross-thread), so invoking a JS callback from a worker thread is a data race
 * on the whole interpreter -- it cannot be made ASan/TSan-clean by any local
 * change here. A correct binding needs a cross-thread hand-off: the worker
 * enqueues the request onto the JS thread, blocks on a condvar, the JS event
 * loop runs the handler and signals completion. That machinery (plus its
 * shutdown/lifetime story) is out of scope for this file and is left as a
 * follow-up. Only HttpClient (fully synchronous, single-threaded) is exposed.
 */

#endif /* CONFIG_SCL_MODULES && CONFIG_SCL_MODULE_HTTP */
