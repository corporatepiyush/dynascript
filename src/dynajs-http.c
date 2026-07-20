/*
 * scl:http -- self-contained, in-repo HTTP/1.1 client + server (no external deps).
 *
 *   import { HttpClient, HttpServer } from "scl:http";
 *
 *   // --- client (synchronous, runs entirely on the JS thread) ---
 *   const c = new HttpClient();
 *   try {
 *     const r = c.get("http://127.0.0.1:8080/");     // { status, statusText,
 *     const p = c.post("http://host/api", '{"a":1}',  //   ok, headers, body }
 *                      { "Content-Type": "application/json" });
 *   } finally { c.close(); }
 *
 *   // --- server (acceptor + worker threads; SAFE BY CONSTRUCTION) ---
 *   const s = new HttpServer({ port: 0, workers: 4, routes: {
 *     "/":     "hello",
 *     "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
 *   }});
 *   s.start();
 *   const port = s.port;   // real bound port (0 => ephemeral, resolved at ctor)
 *   ...
 *   s.close();             // stop() (joins all threads) then frees the table
 *
 * Threading model & the ONE hard rule: the server runs an acceptor thread plus N
 * worker threads (pthreads). A DynaJS JSContext/JSRuntime is single-threaded, so
 * a worker thread must NEVER touch any JSContext/JS_* API -- that would be a data
 * race on the interpreter. We make that impossible by DEEP-COPYING the entire
 * route table into plain malloc'd C memory at construction time (on the JS
 * thread, before any thread starts). Workers only read that immutable-after-start
 * table and use libc malloc/free (thread-safe) for per-connection scratch. A
 * path not in the table gets a 404. stop() joins every thread BEFORE any teardown.
 *
 * Memory model (see dynajs-nat.h): a JS wrapper owns one native pointer freed by
 * its dispose callback. Every method coerces all JS args to C locals FIRST, then
 * resolves the native handle (which rejects a closed resource) -- coercion can run
 * user JS that close()s `this`, so nothing native is touched before the resolve.
 * Native results are copied into fresh JS values at the boundary; no native
 * pointer ever escapes into the JS heap.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Structured error codes surfaced as the numeric `.sclError` on a thrown Error. */
#define DYN_HTTP_ERR_URL      1 /* malformed / unsupported URL */
#define DYN_HTTP_ERR_RESOLVE  2 /* getaddrinfo failed */
#define DYN_HTTP_ERR_CONNECT  3 /* socket/connect failed */
#define DYN_HTTP_ERR_SEND     4 /* send failed */
#define DYN_HTTP_ERR_RECV     5 /* recv failed */
#define DYN_HTTP_ERR_PARSE    6 /* malformed response */
#define DYN_HTTP_ERR_OOM      7 /* out of memory */
#define DYN_HTTP_ERR_TOOBIG   8 /* response exceeded the client cap */

#define DYN_HTTP_DEFAULT_TIMEOUT_MS 15000
#define DYN_HTTP_DEFAULT_MAX_BODY   (16 * 1024 * 1024) /* 16 MB */
#define DYN_HTTP_CONN_QUEUE_CAP     256

/* ==================================================================== *
 *  Small shared helpers (libc only -- reused by client and server)      *
 * ==================================================================== */

/* Grow-only byte buffer backed by libc malloc (thread-safe; used off the JS
 * thread by the server). Returns -1 on OOM. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dyn_bytes_t;

static int dyn_bytes_reserve(dyn_bytes_t *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;
    char *nd;

    if (need <= b->cap)
        return 0;
    nc = b->cap ? b->cap * 2 : 4096;
    while (nc < need)
        nc *= 2;
    nd = (char *)realloc(b->data, nc);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int dyn_bytes_append(dyn_bytes_t *b, const char *src, size_t n)
{
    if (dyn_bytes_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

/* Portable, dependency-free substring search over a possibly-NUL-containing
 * byte range (avoids relying on memmem / _GNU_SOURCE). */
static const char *dyn_memfind(const char *hay, size_t hlen,
                               const char *needle, size_t nlen)
{
    size_t i;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

static int dyn_ci_equal(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        int ca = a[i], cb = b[i];
        if (cb == '\0')
            return 0;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
    }
    return b[i] == '\0';
}

/* Write all `len` bytes to `fd`, retrying short writes. Returns 0 or -1. */
static int dyn_send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(fd, buf + off, len - off, 0);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}

/* ==================================================================== *
 *  HttpClient                                                           *
 * ==================================================================== */

static JSClassID dyn_http_client_class_id;

typedef struct {
    int64_t timeout_ms;  /* recv/connect timeout; <=0 => none */
    size_t max_body;     /* cap on accepted response bytes */
} dyn_http_client_t;

static void dyn_http_client_dispose(void *native)
{
    free(native);
}

static const JSClassDef dyn_http_client_class = {
    "HttpClient",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_http_client_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    int64_t max_body = 0; /* 0 => default */

    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt64(ctx, &max_body, argv[0]))
            return JS_EXCEPTION;
        if (max_body < 0)
            max_body = 0;
    }
    cl = (dyn_http_client_t *)malloc(sizeof(*cl));
    if (!cl)
        return JS_ThrowOutOfMemory(ctx);
    cl->timeout_ms = DYN_HTTP_DEFAULT_TIMEOUT_MS;
    cl->max_body = max_body ? (size_t)max_body : DYN_HTTP_DEFAULT_MAX_BODY;
    return dyn_res_wrap(ctx, dyn_http_client_class_id, cl,
                        dyn_http_client_dispose);
}

/* --- URL parsing: "http://host[:port][/path]" (plain TCP; http only) --- */

typedef struct {
    char host[256];
    uint16_t port;
    char path[2048];
} dyn_url_t;

static int dyn_parse_url(const char *url, dyn_url_t *out)
{
    const char *p = url;
    const char *host_start, *host_end;
    size_t host_len, path_len;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        return -1; /* no TLS in this plain-socket client */
    else
        return -1; /* require an explicit scheme */

    host_start = p;
    while (*p && *p != ':' && *p != '/')
        p++;
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    out->port = 80;
    if (*p == ':') {
        long port = 0;
        p++;
        if (*p < '0' || *p > '9')
            return -1;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            if (port > 65535)
                return -1;
            p++;
        }
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
        return 0;
    }
    if (*p != '/')
        return -1;
    path_len = strlen(p);
    if (path_len >= sizeof(out->path))
        return -1;
    memcpy(out->path, p, path_len + 1);
    return 0;
}

/* Non-blocking connect with a timeout. Returns a connected fd, or -1 with
 * *perr set. */
static int dyn_tcp_connect(const char *host, uint16_t port, int64_t timeout_ms,
                           int *perr)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        *perr = DYN_HTTP_ERR_RESOLVE;
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        int flags, rc;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            int pr;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            pr = poll(&pfd, 1, timeout_ms > 0 ? (int)timeout_ms : -1);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                    soerr == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        *perr = DYN_HTTP_ERR_CONNECT;
    return fd;
}

/* Build the extra-header block ("Name: Value\r\n...") from the JS `headers`
 * argument (a { name: value } object or a pre-formatted string). Returns a
 * malloc'd NUL-terminated string the caller free()s, or NULL. NULL with
 * *perr == 0 means "no extra headers"; NULL with *perr == 1 means a JS
 * exception is pending. Rejects CR/LF in names/values (header injection). */
static char *dyn_headers_to_string(JSContext *ctx, JSValueConst headers,
                                   int *perr)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    dyn_bytes_t buf = {0};

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
        out = (char *)malloc(n + 1);
        if (out)
            memcpy(out, s, n + 1);
        else
            *perr = 1;
        JS_FreeCString(ctx, s);
        return out;
    }

    if (!JS_IsObject(headers))
        return NULL; /* numbers/bools etc.: treat as "no extra headers" */

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const char *val;
        JSValue v;
        size_t nl, vl;

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
        if (memchr(name, '\r', nl) || memchr(name, '\n', nl) ||
            memchr(val, '\r', vl) || memchr(val, '\n', vl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                              "header name/value must not contain CR or LF");
            return NULL;
        }
        if (dyn_bytes_append(&buf, name, nl) < 0 ||
            dyn_bytes_append(&buf, ": ", 2) < 0 ||
            dyn_bytes_append(&buf, val, vl) < 0 ||
            dyn_bytes_append(&buf, "\r\n", 2) < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            goto fail;
        }
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (buf.data) {
        if (dyn_bytes_append(&buf, "\0", 1) < 0) {
            free(buf.data);
            return NULL; /* *perr stays 0: treated as no-headers on OOM */
        }
    }
    return buf.data; /* NUL-terminated, or NULL if no own string keys */

 fail:
    free(buf.data);
    JS_FreePropertyEnum(ctx, tab, len);
    *perr = 1;
    return NULL;
}

/* Turn a code into a thrown JS Error with a numeric `.sclError`. Returns
 * JS_EXCEPTION. */
static JSValue dyn_http_throw(JSContext *ctx, int code, const char *method,
                             const char *url)
{
    static const char *const names[] = {
        "ok", "bad URL", "DNS resolution failed", "connection failed",
        "send failed", "receive failed", "malformed response",
        "out of memory", "response too large"};
    JSValue e;
    char msg[512];
    const char *what = (code >= 0 && code < (int)countof(names))
                           ? names[code] : "error";

    snprintf(msg, sizeof(msg), "HTTP %s %s failed: %s", method,
             url ? url : "", what);
    e = JS_NewError(ctx);
    if (JS_IsException(e))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "sclError", JS_NewInt32(ctx, code),
                              JS_PROP_C_W_E);
    return JS_Throw(ctx, e);
}

/* Parse a single header block (NUL-free byte range between the status line and
 * the blank line) into a fresh JS object. */
static JSValue dyn_headers_object(JSContext *ctx, const char *p, const char *end)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    while (p < end) {
        const char *eol = dyn_memfind(p, (size_t)(end - p), "\r\n", 2);
        const char *line_end = eol ? eol : end;
        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char *val = colon + 1;
            size_t val_len = (size_t)(line_end - val);
            JSAtom key;
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }
            if (name_len > 0) {
                key = JS_NewAtomLen(ctx, p, name_len);
                if (key != JS_ATOM_NULL) {
                    JS_DefinePropertyValue(ctx, obj, key,
                                           JS_NewStringLen(ctx, val, val_len),
                                           JS_PROP_C_W_E);
                    JS_FreeAtom(ctx, key);
                }
            }
        }
        if (!eol)
            break;
        p = eol + 2;
    }
    return obj;
}

/* De-chunk a Transfer-Encoding: chunked body into a fresh malloc'd buffer. */
static char *dyn_dechunk(const char *p, size_t len, size_t *out_len)
{
    dyn_bytes_t out = {0};
    size_t i = 0;

    for (;;) {
        size_t chunk = 0, j = i;
        int digits = 0;
        while (j < len && p[j] != '\r' && p[j] != ';') {
            int c = p[j];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            chunk = chunk * 16 + (size_t)d;
            digits++;
            j++;
        }
        if (!digits)
            break;
        /* skip to end of the chunk-size line */
        while (j < len && p[j] != '\n')
            j++;
        if (j >= len)
            break;
        i = j + 1; /* past the \n */
        if (chunk == 0)
            break; /* last chunk */
        if (i + chunk > len)
            chunk = len - i; /* truncated: copy what we have */
        if (dyn_bytes_append(&out, p + i, chunk) < 0) {
            free(out.data);
            *out_len = 0;
            return NULL;
        }
        i += chunk;
        if (i + 2 <= len)
            i += 2; /* trailing \r\n */
        else
            break;
    }
    *out_len = out.len;
    return out.data; /* may be NULL if body was empty */
}

/* Read the full response off `fd` into `resp`, honouring Content-Length,
 * Transfer-Encoding: chunked, and Connection: close. Returns 0 or an error
 * code. On success *resp owns a malloc'd buffer (caller free()s resp->data). */
static int dyn_read_response(int fd, size_t max_body, dyn_bytes_t *resp,
                             size_t *phdr_end)
{
    const char *hdr_marker;
    size_t hdr_end = 0;

    /* 1. read until end-of-headers */
    for (;;) {
        ssize_t r;
        if (dyn_bytes_reserve(resp, 8192) < 0)
            return DYN_HTTP_ERR_OOM;
        r = recv(fd, resp->data + resp->len, resp->cap - resp->len, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return DYN_HTTP_ERR_RECV;
        }
        if (r == 0)
            return DYN_HTTP_ERR_PARSE; /* closed before full headers */
        resp->len += (size_t)r;
        if (resp->len > max_body)
            return DYN_HTTP_ERR_TOOBIG;
        hdr_marker = dyn_memfind(resp->data, resp->len, "\r\n\r\n", 4);
        if (hdr_marker) {
            hdr_end = (size_t)(hdr_marker - resp->data) + 4;
            break;
        }
    }
    *phdr_end = hdr_end;

    /* 2. inspect headers for framing */
    {
        const char *hstart = dyn_memfind(resp->data, hdr_end, "\r\n", 2);
        const char *hp = hstart ? hstart + 2 : resp->data;
        const char *hend = resp->data + hdr_end - 2; /* before blank line */
        size_t content_length = 0;
        int have_cl = 0, chunked = 0;

        while (hp < hend) {
            const char *eol = dyn_memfind(hp, (size_t)(hend - hp), "\r\n", 2);
            const char *line_end = eol ? eol : hend;
            const char *colon = memchr(hp, ':', (size_t)(line_end - hp));
            if (colon) {
                size_t nlen = (size_t)(colon - hp);
                const char *val = colon + 1;
                size_t vlen = (size_t)(line_end - val);
                while (vlen > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    vlen--;
                }
                if (dyn_ci_equal(hp, nlen, "content-length")) {
                    size_t k;
                    content_length = 0;
                    have_cl = 1;
                    for (k = 0; k < vlen && val[k] >= '0' && val[k] <= '9'; k++)
                        content_length = content_length * 10 +
                                         (size_t)(val[k] - '0');
                } else if (dyn_ci_equal(hp, nlen, "transfer-encoding")) {
                    if (dyn_memfind(val, vlen, "chunked", 7))
                        chunked = 1;
                }
            }
            if (!eol)
                break;
            hp = eol + 2;
        }

        /* 3. read the body per the framing rule */
        if (chunked || !have_cl) {
            /* read until the peer closes (Connection: close) */
            for (;;) {
                ssize_t r;
                if (dyn_bytes_reserve(resp, 8192) < 0)
                    return DYN_HTTP_ERR_OOM;
                r = recv(fd, resp->data + resp->len, resp->cap - resp->len, 0);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    return DYN_HTTP_ERR_RECV;
                }
                if (r == 0)
                    break;
                resp->len += (size_t)r;
                if (resp->len > max_body)
                    return DYN_HTTP_ERR_TOOBIG;
            }
        } else {
            size_t want = hdr_end + content_length;
            if (want > max_body)
                return DYN_HTTP_ERR_TOOBIG;
            while (resp->len < want) {
                ssize_t r;
                if (dyn_bytes_reserve(resp, want - resp->len) < 0)
                    return DYN_HTTP_ERR_OOM;
                r = recv(fd, resp->data + resp->len, resp->cap - resp->len, 0);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    return DYN_HTTP_ERR_RECV;
                }
                if (r == 0)
                    break; /* short body: return what we got */
                resp->len += (size_t)r;
            }
        }
    }
    return 0;
}

/* Parse the accumulated raw response into a JS { status, statusText, ok,
 * headers, body } object. */
static JSValue dyn_build_response(JSContext *ctx, const char *raw, size_t len,
                                  size_t hdr_end)
{
    JSValue obj, headers;
    const char *line_end = dyn_memfind(raw, len, "\r\n", 2);
    const char *sp1, *sp2;
    int status = 0;
    const char *status_text = "";
    size_t status_text_len = 0;
    const char *body;
    size_t body_len;
    char *dechunked = NULL;
    int chunked = 0;

    if (!line_end)
        return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL);

    /* status line: HTTP/1.x <code> <reason> */
    sp1 = memchr(raw, ' ', (size_t)(line_end - raw));
    if (sp1) {
        const char *code = sp1 + 1;
        while (code < line_end && *code >= '0' && *code <= '9') {
            status = status * 10 + (*code - '0');
            code++;
        }
        sp2 = (code < line_end && *code == ' ') ? code + 1 : code;
        status_text = sp2;
        status_text_len = (size_t)(line_end - sp2);
    }

    /* detect chunked to know how to expose the body */
    {
        const char *hp = line_end + 2;
        const char *hend = raw + hdr_end - 2;
        while (hp < hend) {
            const char *eol = dyn_memfind(hp, (size_t)(hend - hp), "\r\n", 2);
            const char *le = eol ? eol : hend;
            const char *colon = memchr(hp, ':', (size_t)(le - hp));
            if (colon) {
                size_t nlen = (size_t)(colon - hp);
                const char *val = colon + 1;
                size_t vlen = (size_t)(le - val);
                while (vlen > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    vlen--;
                }
                if (dyn_ci_equal(hp, nlen, "transfer-encoding") &&
                    dyn_memfind(val, vlen, "chunked", 7))
                    chunked = 1;
            }
            if (!eol)
                break;
            hp = eol + 2;
        }
    }

    body = raw + hdr_end;
    body_len = len - hdr_end;
    if (chunked) {
        size_t dl = 0;
        dechunked = dyn_dechunk(body, body_len, &dl);
        body = dechunked;
        body_len = dl;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        free(dechunked);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "status", JS_NewInt32(ctx, status),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
                              JS_NewStringLen(ctx, status_text, status_text_len),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok",
                              JS_NewBool(ctx, status >= 200 && status < 300),
                              JS_PROP_C_W_E);
    headers = dyn_headers_object(ctx, line_end + 2, raw + hdr_end - 2);
    if (JS_IsException(headers)) {
        free(dechunked);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, obj, "headers", headers, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "body",
                              JS_NewStringLen(ctx, body ? body : "", body_len),
                              JS_PROP_C_W_E);
    free(dechunked);
    return obj;
}

/* Shared request driver. All JS args are coerced to C locals BEFORE the native
 * handle is resolved. */
static JSValue dyn_http_perform(JSContext *ctx, JSValueConst this_val,
                                const char *method, JSValueConst url_val,
                                JSValueConst body_val, JSValueConst headers_val)
{
    dyn_http_client_t *cl;
    const char *url = NULL;
    const char *body = NULL;
    size_t body_len = 0;
    char *hdr = NULL;
    int hdr_err = 0;
    dyn_url_t u;
    dyn_bytes_t req = {0};
    dyn_bytes_t resp = {0};
    int fd = -1, err = 0;
    size_t hdr_end = 0;
    char line[512];
    JSValue result;

    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");

    /* --- coerce every JS arg first (may run user valueOf/toString) --- */
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
    hdr = dyn_headers_to_string(ctx, headers_val, &hdr_err);
    if (hdr_err) {
        if (body)
            JS_FreeCString(ctx, body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    /* --- resolve the client AFTER all coercions (rejects a closed client) --- */
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl) {
        free(hdr);
        if (body)
            JS_FreeCString(ctx, body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    if (dyn_parse_url(url, &u) < 0) {
        err = DYN_HTTP_ERR_URL;
        goto done;
    }

    /* fresh connection per request (simplest correct behaviour) */
    fd = dyn_tcp_connect(u.host, u.port, cl->timeout_ms, &err);
    if (fd < 0)
        goto done;
    if (cl->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = cl->timeout_ms / 1000;
        tv.tv_usec = (cl->timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* build the request */
    if (u.port == 80)
        snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s\r\n",
                 method, u.path, u.host);
    else
        snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s:%u\r\n",
                 method, u.path, u.host, (unsigned)u.port);
    if (dyn_bytes_append(&req, line, strlen(line)) < 0) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }
    if (hdr && dyn_bytes_append(&req, hdr, strlen(hdr)) < 0) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }
    if (body) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
        if (dyn_bytes_append(&req, line, strlen(line)) < 0) {
            err = DYN_HTTP_ERR_OOM;
            goto done;
        }
    }
    if (dyn_bytes_append(&req, "Connection: close\r\n\r\n", 21) < 0 ||
        (body && dyn_bytes_append(&req, body, body_len) < 0)) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }

    if (dyn_send_all(fd, req.data, req.len) < 0) {
        err = DYN_HTTP_ERR_SEND;
        goto done;
    }
    err = dyn_read_response(fd, cl->max_body, &resp, &hdr_end);

 done:
    if (fd >= 0)
        close(fd);
    free(req.data);
    free(hdr);
    if (body)
        JS_FreeCString(ctx, body);

    if (err != 0) {
        free(resp.data);
        result = dyn_http_throw(ctx, err, method, url);
    } else {
        result = dyn_build_response(ctx, resp.data, resp.len, hdr_end);
        free(resp.data);
    }
    JS_FreeCString(ctx, url);
    return result;
}

static JSValue dyn_http_client_get(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return dyn_http_perform(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
                            argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue dyn_http_client_post(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return dyn_http_perform(ctx, this_val, "POST", argv[0],
                            argc > 1 ? argv[1] : JS_UNDEFINED,
                            argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue dyn_http_client_request(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "request(method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = dyn_http_perform(ctx, this_val, method, argv[1],
                           argc > 2 ? argv[2] : JS_UNDEFINED,
                           argc > 3 ? argv[3] : JS_UNDEFINED);
    JS_FreeCString(ctx, method);
    return res;
}

static JSValue dyn_http_client_set_timeout(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    int64_t ms;

    (void)argc;
    if (JS_ToInt64(ctx, &ms, argv[0])) /* coerce FIRST (may close `this`) */
        return JS_EXCEPTION;
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    cl->timeout_ms = ms;
    return JS_UNDEFINED;
}

static JSValue dyn_http_client_disconnect(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    /* This client opens a fresh connection per request and holds no socket, so
     * disconnect() is a no-op kept for API compatibility. Still validate. */
    dyn_http_client_t *cl;
    (void)argc; (void)argv;
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_http_client_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_http_client_get),
    JS_CFUNC_DEF("post", 2, dyn_http_client_post),
    JS_CFUNC_DEF("request", 2, dyn_http_client_request),
    JS_CFUNC_DEF("setTimeout", 1, dyn_http_client_set_timeout),
    JS_CFUNC_DEF("disconnect", 0, dyn_http_client_disconnect),
};

/* ==================================================================== *
 *  HttpServer                                                           *
 * ==================================================================== */

static JSClassID dyn_http_server_class_id;

/* One deep-copied route: all bytes are libc-malloc'd C memory, immutable after
 * the server's threads start. Workers only ever READ these. */
typedef struct {
    char *path;
    int status;
    char *content_type;
    char *body;
    size_t body_len;
} dyn_route_t;

/* Bounded MPMC fd queue: acceptor pushes, workers pop. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    int fds[DYN_HTTP_CONN_QUEUE_CAP];
    int head, tail, count;
    int shutdown; /* all guarded by mutex */
} dyn_conn_queue_t;

typedef struct {
    int listen_fd;
    uint16_t port;
    int backlog;
    int num_workers;

    dyn_route_t *routes; /* immutable after start */
    size_t n_routes;

    dyn_conn_queue_t q;
    pthread_t acceptor;
    pthread_t *workers;
    int started;             /* JS-thread only */
    atomic_int stop_flag;    /* acceptor loop breaks on this */
} dyn_http_server_t;

static const char *dyn_reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

/* --- worker/acceptor code: NO JS_* CALLS BELOW (different threads) --- */

/* Extract the request-target path (query stripped) from the request line. */
static int dyn_parse_req_path(const char *buf, size_t len, char *out,
                              size_t outsz)
{
    size_t i = 0, start, plen;

    while (i < len && buf[i] != ' ')
        i++; /* skip method */
    if (i >= len)
        return -1;
    i++;
    start = i;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '?')
        i++;
    if (i >= len)
        return -1;
    plen = i - start;
    if (plen == 0)
        return -1;
    if (plen >= outsz)
        plen = outsz - 1;
    memcpy(out, buf + start, plen);
    out[plen] = '\0';
    return 0;
}

static const dyn_route_t *dyn_route_lookup(const dyn_http_server_t *s,
                                           const char *path)
{
    size_t i;
    for (i = 0; i < s->n_routes; i++) {
        if (strcmp(s->routes[i].path, path) == 0)
            return &s->routes[i];
    }
    return NULL;
}

/* Case-insensitive byte compare of `n` bytes. */
static int dyn_ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

/* Find request header `name` (case-insensitive) in the header block
 * [buf, buf+len). Every header line is preceded by CRLF (the request line is
 * not, so it never false-matches). Returns the value with leading spaces
 * trimmed and sets *vlen, or NULL if absent. */
static const char *dyn_req_header(const char *buf, size_t len,
                                  const char *name, size_t *vlen)
{
    size_t nlen = strlen(name), i;
    for (i = 0; i + 2 + nlen + 1 <= len; i++) {
        const char *v, *ve, *end;
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;
        if (!dyn_ci_eq(buf + i + 2, name, nlen) || buf[i + 2 + nlen] != ':')
            continue;
        v = buf + i + 2 + nlen + 1;
        end = buf + len;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        ve = v;
        while (ve < end && *ve != '\r' && *ve != '\n')
            ve++;
        *vlen = (size_t)(ve - v);
        return v;
    }
    return NULL;
}

/* True if header value [v,v+vlen) case-insensitively contains token `tok`. */
static int dyn_val_has_token(const char *v, size_t vlen, const char *tok)
{
    size_t tlen = strlen(tok), i;
    if (!v)
        return 0;
    for (i = 0; i + tlen <= vlen; i++)
        if (dyn_ci_eq(v + i, tok, tlen))
            return 1;
    return 0;
}

static void dyn_http_respond(int fd, int status, const char *content_type,
                             const char *body, size_t body_len, int keep_alive)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, dyn_reason_phrase(status),
                     content_type ? content_type : "text/plain", body_len,
                     keep_alive ? "keep-alive" : "close");
    if (n < 0 || n >= (int)sizeof(head))
        return;
    if (dyn_send_all(fd, head, (size_t)n) < 0)
        return;
    if (body_len > 0)
        dyn_send_all(fd, body, body_len);
}

/* Serve a connection with HTTP/1.1 keep-alive: read requests back-to-back on
 * the same socket until the client asks to close, a request is malformed/too
 * large, the per-connection cap is hit, or the socket idles past the recv
 * timeout. Each request is fully consumed (header block + Content-Length body)
 * so pipelined bytes never desync the framing of the next request. */
static void dyn_http_handle_conn(const dyn_http_server_t *s, int fd)
{
    char reqbuf[16384];
    size_t rlen = 0;
    char path[2048];
    struct timeval tv;
    int nreq = 0;
    const int max_req = 10000;

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        const char *hdr_end, *cl, *conn;
        size_t head_len, body_len = 0, req_total, clv_len = 0, connv_len = 0;
        int http11, keep_alive;
        const dyn_route_t *route;

        /* read until the header terminator, reusing buffered (pipelined) bytes */
        while (!(hdr_end = dyn_memfind(reqbuf, rlen, "\r\n\r\n", 4))) {
            ssize_t r;
            if (rlen >= sizeof(reqbuf) - 1)
                return; /* header block too large: drop the connection */
            r = recv(fd, reqbuf + rlen, sizeof(reqbuf) - 1 - rlen, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                return; /* EOF / idle timeout / error */
            }
            rlen += (size_t)r;
        }
        head_len = (size_t)(hdr_end - reqbuf) + 4;

        /* keep-alive decision: HTTP/1.1 defaults to keep-alive, HTTP/1.0 to
         * close; an explicit Connection header overrides. */
        http11 = dyn_memfind(reqbuf, head_len, "HTTP/1.1\r\n", 10) != NULL;
        conn = dyn_req_header(reqbuf, head_len, "connection", &connv_len);
        keep_alive = http11;
        if (dyn_val_has_token(conn, connv_len, "close"))
            keep_alive = 0;
        else if (dyn_val_has_token(conn, connv_len, "keep-alive"))
            keep_alive = 1;

        /* consume the request body (routes ignore it, but it must not desync) */
        cl = dyn_req_header(reqbuf, head_len, "content-length", &clv_len);
        if (cl) {
            size_t j;
            for (j = 0; j < clv_len && cl[j] >= '0' && cl[j] <= '9'; j++)
                body_len = body_len * 10 + (size_t)(cl[j] - '0');
        }
        req_total = head_len + body_len;
        if (req_total > sizeof(reqbuf) - 1)
            return; /* request too large to frame for keep-alive: drop */
        while (rlen < req_total) {
            ssize_t r = recv(fd, reqbuf + rlen, sizeof(reqbuf) - 1 - rlen, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                return;
            }
            rlen += (size_t)r;
        }

        if (++nreq >= max_req)
            keep_alive = 0; /* cap requests per connection */

        if (dyn_parse_req_path(reqbuf, head_len, path, sizeof(path)) < 0) {
            dyn_http_respond(fd, 400, "text/plain", "Bad Request", 11, 0);
            return;
        }
        route = dyn_route_lookup(s, path);
        if (route)
            dyn_http_respond(fd, route->status, route->content_type,
                             route->body, route->body_len, keep_alive);
        else
            dyn_http_respond(fd, 404, "text/plain", "Not Found", 9, keep_alive);

        if (!keep_alive)
            return;

        /* carry pipelined bytes beyond this request to the next iteration */
        memmove(reqbuf, reqbuf + req_total, rlen - req_total);
        rlen -= req_total;
    }
}

static void *dyn_http_worker_main(void *arg)
{
    dyn_http_server_t *s = (dyn_http_server_t *)arg;
    for (;;) {
        int fd;
        pthread_mutex_lock(&s->q.mutex);
        while (s->q.count == 0 && !s->q.shutdown)
            pthread_cond_wait(&s->q.not_empty, &s->q.mutex);
        if (s->q.count == 0 && s->q.shutdown) {
            pthread_mutex_unlock(&s->q.mutex);
            break;
        }
        fd = s->q.fds[s->q.head];
        s->q.head = (s->q.head + 1) % DYN_HTTP_CONN_QUEUE_CAP;
        s->q.count--;
        pthread_mutex_unlock(&s->q.mutex);

        dyn_http_handle_conn(s, fd);
        close(fd);
    }
    return NULL;
}

static void *dyn_http_acceptor_main(void *arg)
{
    dyn_http_server_t *s = (dyn_http_server_t *)arg;
    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        struct pollfd pfd;
        int pr, fd;
        pfd.fd = s->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 200); /* tick so we notice stop_flag */
        if (pr <= 0)
            continue;
        fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&s->q.mutex);
        if (s->q.count < DYN_HTTP_CONN_QUEUE_CAP && !s->q.shutdown) {
            s->q.fds[s->q.tail] = fd;
            s->q.tail = (s->q.tail + 1) % DYN_HTTP_CONN_QUEUE_CAP;
            s->q.count++;
            pthread_cond_signal(&s->q.not_empty);
            pthread_mutex_unlock(&s->q.mutex);
        } else {
            pthread_mutex_unlock(&s->q.mutex);
            close(fd); /* queue full or shutting down: drop */
        }
    }
    return NULL;
}

/* --- lifecycle (JS thread only) --- */

/* Idempotent: join all threads (BEFORE any teardown) if running. */
static void dyn_http_server_stop_internal(dyn_http_server_t *s)
{
    int i;
    if (!s->started)
        return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    pthread_join(s->acceptor, NULL);
    pthread_mutex_lock(&s->q.mutex);
    s->q.shutdown = 1;
    pthread_cond_broadcast(&s->q.not_empty);
    pthread_mutex_unlock(&s->q.mutex);
    for (i = 0; i < s->num_workers; i++)
        pthread_join(s->workers[i], NULL);
    /* every thread is joined: drain any leftover fds without locking */
    while (s->q.count > 0) {
        close(s->q.fds[s->q.head]);
        s->q.head = (s->q.head + 1) % DYN_HTTP_CONN_QUEUE_CAP;
        s->q.count--;
    }
    s->started = 0;
}

static int dyn_http_server_spawn(dyn_http_server_t *s)
{
    int i;
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    s->q.shutdown = 0;
    for (i = 0; i < s->num_workers; i++) {
        if (pthread_create(&s->workers[i], NULL, dyn_http_worker_main, s) != 0) {
            /* wake and join the workers already created, then fail */
            pthread_mutex_lock(&s->q.mutex);
            s->q.shutdown = 1;
            pthread_cond_broadcast(&s->q.not_empty);
            pthread_mutex_unlock(&s->q.mutex);
            while (--i >= 0)
                pthread_join(s->workers[i], NULL);
            return -1;
        }
    }
    if (pthread_create(&s->acceptor, NULL, dyn_http_acceptor_main, s) != 0) {
        pthread_mutex_lock(&s->q.mutex);
        s->q.shutdown = 1;
        pthread_cond_broadcast(&s->q.not_empty);
        pthread_mutex_unlock(&s->q.mutex);
        for (i = 0; i < s->num_workers; i++)
            pthread_join(s->workers[i], NULL);
        return -1;
    }
    s->started = 1;
    return 0;
}

static void dyn_http_server_dispose(void *native)
{
    dyn_http_server_t *s = (dyn_http_server_t *)native;
    size_t i;

    dyn_http_server_stop_internal(s); /* join BEFORE freeing anything */
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    if (s->routes) {
        for (i = 0; i < s->n_routes; i++) {
            free(s->routes[i].path);
            free(s->routes[i].content_type);
            free(s->routes[i].body);
        }
        free(s->routes);
    }
    free(s->workers);
    pthread_mutex_destroy(&s->q.mutex);
    pthread_cond_destroy(&s->q.not_empty);
    free(s);
}

/* Deep-copy one route value (string => 200/text/plain; object => {status,
 * contentType, body}) into `r`. Runs on the JS thread. Returns 0 or -1
 * (exception pending). */
static int dyn_route_copy(JSContext *ctx, const char *path, JSValueConst val,
                          dyn_route_t *r)
{
    const char *body = NULL, *ct = NULL;
    size_t body_len = 0;
    int32_t status = 200;

    r->path = NULL;
    r->content_type = NULL;
    r->body = NULL;
    r->body_len = 0;
    r->status = 200;

    if (JS_IsString(val)) {
        body = JS_ToCStringLen(ctx, &body_len, val);
        if (!body)
            return -1;
        ct = NULL; /* default text/plain */
    } else if (JS_IsObject(val)) {
        JSValue vs = JS_GetPropertyStr(ctx, val, "status");
        JSValue vc = JS_GetPropertyStr(ctx, val, "contentType");
        JSValue vb = JS_GetPropertyStr(ctx, val, "body");
        if (!JS_IsUndefined(vs) && !JS_IsNull(vs)) {
            if (JS_ToInt32(ctx, &status, vs)) {
                JS_FreeValue(ctx, vs);
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
        }
        JS_FreeValue(ctx, vs);
        if (!JS_IsUndefined(vc) && !JS_IsNull(vc)) {
            ct = JS_ToCString(ctx, vc);
            if (!ct) {
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
        }
        JS_FreeValue(ctx, vc);
        if (!JS_IsUndefined(vb) && !JS_IsNull(vb)) {
            body = JS_ToCStringLen(ctx, &body_len, vb);
            if (!body) {
                if (ct)
                    JS_FreeCString(ctx, ct);
                JS_FreeValue(ctx, vb);
                return -1;
            }
        }
        JS_FreeValue(ctx, vb);
    } else {
        JS_ThrowTypeError(ctx, "route value must be a string or an object");
        return -1;
    }

    r->status = status;
    r->path = strdup(path);
    r->content_type = strdup(ct ? ct : "text/plain");
    r->body = (char *)malloc(body_len + 1);
    if (!r->path || !r->content_type || !r->body) {
        if (body)
            JS_FreeCString(ctx, body);
        if (ct)
            JS_FreeCString(ctx, ct);
        free(r->path);
        free(r->content_type);
        free(r->body);
        r->path = r->content_type = r->body = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (body_len > 0)
        memcpy(r->body, body, body_len);
    r->body[body_len] = '\0';
    r->body_len = body_len;
    if (body)
        JS_FreeCString(ctx, body);
    if (ct)
        JS_FreeCString(ctx, ct);
    return 0;
}

/* Bind an IPv4 listening socket (SO_REUSEADDR). host==NULL => all interfaces.
 * Resolves an ephemeral port when `*pport`==0. Returns the fd or -1. */
static int dyn_http_bind(const char *host, uint16_t *pport, int backlog)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, on = 1;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)*pport);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? 0 : AI_PASSIVE;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    if (*pport == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0)
            *pport = ntohs(sin.sin_port);
    }
    return fd;
}

static JSValue dyn_http_server_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    dyn_http_server_t *s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char *host_c = NULL;
    char *host_dup = NULL;
    int32_t port = 0, workers = 4, backlog = 0;
    JSPropertyEnum *tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    (void)new_target;
    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    /* --- coerce every option to a C local FIRST (may run user JS) --- */
    if (JS_IsObject(opts)) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &workers, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            host_c = JS_ToCString(ctx, v);
            if (!host_c) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(host_c);
            JS_FreeCString(ctx, host_c);
        }
        JS_FreeValue(ctx, v);

        routes_val = JS_GetPropertyStr(ctx, opts, "routes"); /* owned */
        if (JS_IsException(routes_val)) {
            free(host_dup);
            return JS_EXCEPTION;
        }
    }

    if (port < 0 || port > 65535) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowRangeError(ctx, "port must be in [0, 65535]");
    }
    if (workers < 1)
        workers = 1;
    if (workers > 64)
        workers = 64;
    if (backlog <= 0)
        backlog = SOMAXCONN;

    s = (dyn_http_server_t *)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->num_workers = workers;
    s->backlog = backlog;
    atomic_init(&s->stop_flag, 0);
    pthread_mutex_init(&s->q.mutex, NULL);
    pthread_cond_init(&s->q.not_empty, NULL);
    s->workers = (pthread_t *)calloc((size_t)workers, sizeof(pthread_t));
    if (!s->workers)
        goto oom;

    /* --- DEEP-COPY the route table into C memory (JS thread, pre-start) --- */
    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t *)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char *path = JS_AtomToCString(ctx, tab[i].atom);
            JSValue rv;
            if (!path)
                goto fail_enum;
            rv = JS_GetProperty(ctx, routes_val, tab[i].atom);
            if (JS_IsException(rv)) {
                JS_FreeCString(ctx, path);
                goto fail_enum;
            }
            if (dyn_route_copy(ctx, path, rv, &s->routes[s->n_routes]) < 0) {
                JS_FreeCString(ctx, path);
                JS_FreeValue(ctx, rv);
                goto fail_enum;
            }
            s->n_routes++;
            JS_FreeCString(ctx, path);
            JS_FreeValue(ctx, rv);
        }
        JS_FreePropertyEnum(ctx, tab, n_routes);
        tab = NULL;
    }
    JS_FreeValue(ctx, routes_val);
    routes_val = JS_UNDEFINED;

    /* --- bind now so port 0 resolves at construction --- */
    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind(host_dup, &bound_port, backlog);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        /* dispose frees routes/workers/mutex; hand it a consistent object */
        dyn_http_server_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, dyn_http_server_class_id, s,
                        dyn_http_server_dispose);

 oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
 oom:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_server_dispose(s);
    return JS_ThrowOutOfMemory(ctx);

 fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
 fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_server_dispose(s);
    return JS_EXCEPTION;
}

static JSValue dyn_http_server_start(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    if (dyn_http_server_spawn(s) < 0)
        return JS_ThrowInternalError(ctx, "failed to start HTTP server threads");
    return JS_UNDEFINED;
}

static JSValue dyn_http_server_stop(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_server_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_server_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_http_server_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_server_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_server_stop),
    JS_CGETSET_DEF("port", dyn_http_server_get_port, NULL),
};

static const JSClassDef dyn_http_server_class = {
    "HttpServer",
    .finalizer = dyn_res_finalizer,
};

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

static int dyn_http_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_http_client_class_id,
                           &dyn_http_client_class, dyn_http_client_proto,
                           countof(dyn_http_client_proto),
                           dyn_http_client_ctor, "HttpClient") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_http_server_class_id,
                           &dyn_http_server_class, dyn_http_server_proto,
                           countof(dyn_http_server_proto),
                           dyn_http_server_ctor, "HttpServer") < 0)
        return -1;
    return 0;
}

int js_nat_init_http(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "scl:http", dyn_http_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "HttpClient");
    JS_AddModuleExport(ctx, m, "HttpServer");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_HTTP */
