/*
 * dyna:http -- self-contained, in-repo HTTP/1.1 client + server (no external deps).
 *
 *   import { HttpClient, HttpServer } from "dyna:http";
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
 * Memory model (see dyna-nat.h): a JS wrapper owns one native pointer freed by
 * its dispose callback. Every method coerces all JS args to C locals FIRST, then
 * resolves the native handle (which rejects a closed resource) -- coercion can run
 * user JS that close()s `this`, so nothing native is touched before the resolve.
 * Native results are copied into fresh JS values at the boundary; no native
 * pointer ever escapes into the JS heap.
 */
#include "dyna-nat.h"
#include "dyna-evloop.h"
#include "dyna-aio.h"

/* from dyna-libc.h (not included here): fold a reactor into the JS event loop */
void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata);

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

#if defined(__linux__) && defined(CONFIG_IO_URING)
#include <liburing.h>
#define DYN_HTTP_HAVE_URING 1
#endif

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
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "dyna-simd-kernels.h" /* shared multi-ISA `simd` table (strfind) */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Structured error codes surfaced as the numeric `.dynajsError` on a thrown Error. */
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
 * byte range (avoids relying on memmem / _GNU_SOURCE). Delegates to the shared
 * multi-ISA SIMD strfind kernel (Muła first+last): request/response header
 * blocks are hundreds of bytes, long enough that the vectorised scan beats the
 * scalar byte loop ~4.75x on a realistic header set (see tests/bench_memfind.c).
 * Pure C, no JS -- safe to call from the acceptor/worker threads. The `simd`
 * table is installed once (simd_init) on the JS thread before any thread spawns
 * or any request is parsed. */
static const char *dyn_memfind(const char *hay, size_t hlen,
                               const char *needle, size_t nlen)
{
    size_t idx;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    idx = simd.strfind((const uint8_t *)hay, hlen,
                       (const uint8_t *)needle, nlen);
    return idx == SIZE_MAX ? NULL : hay + idx;
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
/* Disable Nagle's algorithm: an HTTP request/response is a ping-pong of small
 * writes, where Nagle + delayed-ACK can stall a reply ~40ms waiting to coalesce.
 * Every serious HTTP endpoint sets this on both accepted and client sockets.
 * Best-effort (ignore failure: a non-TCP fd or unsupported option is harmless). */
static void dyn_set_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

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
            dyn_set_nodelay(fd);
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
                    dyn_set_nodelay(fd);
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

/* Turn a code into a thrown JS Error with a numeric `.dynajsError`. Returns
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
    JS_DefinePropertyValueStr(ctx, e, "dynajsError", JS_NewInt32(ctx, code),
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

static const dyn_route_t *dyn_route_lookup(const dyn_route_t *routes,
                                           size_t n_routes, const char *path)
{
    size_t i;
    for (i = 0; i < n_routes; i++) {
        if (strcmp(routes[i].path, path) == 0)
            return &routes[i];
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
        route = dyn_route_lookup(s->routes, s->n_routes, path);
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
        dyn_set_nodelay(fd);
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
 *  HttpServerAsync -- Model A: single-threaded kqueue/epoll reactor      *
 *                                                                        *
 *  One background reactor thread multiplexes ALL connections with non-   *
 *  blocking sockets (no per-connection thread, no fd queue). Native      *
 *  static-route handlers only -- the reactor thread never touches the    *
 *  JSContext, so it is safe by construction exactly like the thread-pool *
 *  server. Removes the worker cap and the queue-full connection drops:   *
 *  connection count is bounded only by the fd limit. This is the C-speed *
 *  ceiling the JS-handler path (Model B) is measured against.            *
 * ==================================================================== */

static JSClassID dyn_http_async_class_id;

#define DYN_ACONN_MAX_REQ   (1 * 1024 * 1024) /* cap a single buffered request */
#define DYN_ACONN_MAX_REQS  100000            /* keep-alive requests/connection */

typedef struct dyn_http_async dyn_http_async_t;

/* Per-connection non-blocking state machine. Owns its two buffers. */
typedef struct {
    dyn_http_async_t *srv;
    int fd;
    dyn_bytes_t in;   /* accumulated request bytes (may hold pipelined extras) */
    dyn_bytes_t out;  /* queued response bytes */
    size_t out_off;   /* bytes of `out` already sent */
    int closing;      /* close once `out` is fully flushed */
    int nreq;
} dyn_aconn_t;

struct dyn_http_async {
    int listen_fd;
    uint16_t port;
    int backlog;

    dyn_route_t *routes; /* immutable after start */
    size_t n_routes;

    dyn_evloop_t *loop;      /* created on the reactor thread (readiness path) */
    void *uring;             /* dyn_uring_ctx* when the io_uring reactor is live */
    pthread_t reactor;
    int started;             /* JS-thread only */
    atomic_int stop_flag;    /* reactor loop breaks on this */
    atomic_int spawn_ok;     /* reactor published its loop init result */
};

static int dyn_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Append a full HTTP/1.1 response (head + body) into `out`. Returns 0 or -1. */
static int dyn_http_format(dyn_bytes_t *out, int status, const char *ct,
                           const char *body, size_t body_len, int keep_alive)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, dyn_reason_phrase(status), ct ? ct : "text/plain",
                     body_len, keep_alive ? "keep-alive" : "close");
    if (n < 0 || n >= (int)sizeof(head))
        return -1;
    if (dyn_bytes_append(out, head, (size_t)n) < 0)
        return -1;
    if (body_len > 0 && dyn_bytes_append(out, body, body_len) < 0)
        return -1;
    return 0;
}

static void dyn_aconn_free(dyn_aconn_t *c)
{
    free(c->in.data);
    free(c->out.data);
    free(c);
}

/* Drop a connection: unregister, close, free. */
static void dyn_aconn_close(dyn_evloop_t *lp, dyn_aconn_t *c)
{
    dyn_evloop_del(lp, c->fd);
    close(c->fd);
    dyn_aconn_free(c);
}

/* Parse as many complete pipelined requests as `c->in` holds, appending a
 * response for each to `c->out`. Returns 1 if the connection should close after
 * the queued output drains, 0 to keep reading. */
/* Core request pump shared by the readiness (Model A) and io_uring completion
 * reactors: parse every complete pipelined request buffered in `in`, appending
 * a response for each to `out`, and consume it from `in`. *pnreq counts
 * requests served on this connection (keep-alive cap). Returns 1 if the
 * connection should close once `out` drains, else 0. */
static int dyn_http_pump(dyn_bytes_t *in, dyn_bytes_t *out, int *pnreq,
                         const dyn_route_t *routes, size_t n_routes)
{
    char path[2048];

    for (;;) {
        const char *base = in->data;
        size_t avail = in->len;
        const char *hdr_end = dyn_memfind(base, avail, "\r\n\r\n", 4);
        size_t head_len, body_len = 0, req_total, clv_len = 0, connv_len = 0;
        const char *cl, *conn;
        int http11, keep_alive;
        const dyn_route_t *route;

        if (!hdr_end) {
            if (avail > DYN_ACONN_MAX_REQ)
                return 1; /* header block too large: drop */
            return 0;     /* need more bytes */
        }
        head_len = (size_t)(hdr_end - base) + 4;

        cl = dyn_req_header(base, head_len, "content-length", &clv_len);
        if (cl) {
            size_t j;
            for (j = 0; j < clv_len && cl[j] >= '0' && cl[j] <= '9'; j++)
                body_len = body_len * 10 + (size_t)(cl[j] - '0');
        }
        req_total = head_len + body_len;
        if (req_total > DYN_ACONN_MAX_REQ) {
            dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
            return 1;
        }
        if (avail < req_total)
            return 0; /* body not fully arrived yet */

        http11 = dyn_memfind(base, head_len, "HTTP/1.1\r\n", 10) != NULL;
        conn = dyn_req_header(base, head_len, "connection", &connv_len);
        keep_alive = http11;
        if (dyn_val_has_token(conn, connv_len, "close"))
            keep_alive = 0;
        else if (dyn_val_has_token(conn, connv_len, "keep-alive"))
            keep_alive = 1;
        if (++(*pnreq) >= DYN_ACONN_MAX_REQS)
            keep_alive = 0;

        if (dyn_parse_req_path(base, head_len, path, sizeof(path)) < 0) {
            dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
            return 1;
        }
        route = dyn_route_lookup(routes, n_routes, path);
        if (route)
            dyn_http_format(out, route->status, route->content_type,
                            route->body, route->body_len, keep_alive);
        else
            dyn_http_format(out, 404, "text/plain", "Not Found", 9, keep_alive);

        /* drop the consumed request; keep any pipelined trailing bytes */
        memmove(in->data, in->data + req_total, in->len - req_total);
        in->len -= req_total;
        if (!keep_alive)
            return 1;
    }
}

static int dyn_aconn_process(dyn_aconn_t *c)
{
    return dyn_http_pump(&c->in, &c->out, &c->nreq, c->srv->routes,
                         c->srv->n_routes);
}

/* Attempt to flush `c->out`; on full drain either close or resume reading.
 * Returns 0 if the connection lives, 1 if it was closed/freed. */
static int dyn_aconn_flush(dyn_evloop_t *lp, dyn_aconn_t *c)
{
    while (c->out_off < c->out.len) {
        ssize_t w = send(c->fd, c->out.data + c->out_off,
                         c->out.len - c->out_off, 0);
        if (w > 0) {
            c->out_off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            dyn_evloop_mod(lp, c->fd, DYN_EV_WRITE); /* wait to drain */
            return 0;
        }
        dyn_aconn_close(lp, c); /* peer gone / hard error */
        return 1;
    }
    /* fully sent */
    c->out.len = 0;
    c->out_off = 0;
    if (c->closing) {
        dyn_aconn_close(lp, c);
        return 1;
    }
    dyn_evloop_mod(lp, c->fd, DYN_EV_READ);
    return 0;
}

static void dyn_aconn_cb(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_aconn_t *c = (dyn_aconn_t *)udata;
    (void)fd;

    if (events & DYN_EV_WRITE) {
        if (dyn_aconn_flush(lp, c))
            return;
    }
    if (events & DYN_EV_READ) {
        for (;;) {
            ssize_t r;
            if (dyn_bytes_reserve(&c->in, 16384) < 0) {
                dyn_aconn_close(lp, c);
                return;
            }
            r = recv(fd, c->in.data + c->in.len, c->in.cap - c->in.len, 0);
            if (r > 0) {
                c->in.len += (size_t)r;
                if (c->in.len > DYN_ACONN_MAX_REQ) /* wildly oversized: drop */
                    { dyn_aconn_close(lp, c); return; }
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break; /* drained the socket buffer */
            /* r == 0 (peer half/closed its write side) or a hard error: no more
             * input will arrive. Fall through to process whatever is already
             * buffered (a client may send a full request then shutdown(SHUT_WR)
             * while still waiting to read the reply), flush it, then close. */
            c->closing = 1;
            break;
        }
        if (dyn_aconn_process(c))
            c->closing = 1;
        if (c->out.len > c->out_off) {
            if (dyn_aconn_flush(lp, c))
                return;
        } else if (c->closing) {
            dyn_aconn_close(lp, c);
            return;
        }
    }
    if (events & DYN_EV_ERROR) {
        if (c->out_off >= c->out.len)
            dyn_aconn_close(lp, c);
    }
}

static void dyn_alisten_cb(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_http_async_t *s = (dyn_http_async_t *)udata;
    (void)events;
    for (;;) {
        dyn_aconn_t *c;
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break; /* EAGAIN: no more pending connections */
        }
        if (dyn_set_nonblock(cfd) < 0) {
            close(cfd);
            continue;
        }
        dyn_set_nodelay(cfd);
        c = (dyn_aconn_t *)calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        c->srv = s;
        c->fd = cfd;
        if (dyn_evloop_add(lp, cfd, DYN_EV_READ, dyn_aconn_cb, c) < 0) {
            close(cfd);
            dyn_aconn_free(c);
        }
    }
}

#ifdef DYN_HTTP_HAVE_URING
/* ==================================================================== *
 *  io_uring completion-model reactor (Linux, CONFIG_IO_URING)           *
 *                                                                        *
 *  NOT the poll-mode readiness shim in dyna-evloop.c. This is the real *
 *  io_uring recipe: multishot accept + multishot recv fed from a SHARED  *
 *  provided-buffer ring (idle connections hold zero buffer memory) +     *
 *  batched send, driven by SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN so   *
 *  the whole submit/complete cycle is one syscall. Data is delivered in  *
 *  completions -- there is no per-fd readiness poll and no recv/accept   *
 *  syscall per event.                                                    *
 * ==================================================================== */

#define DYN_URING_ENTRIES 8192
#define DYN_URING_NBUFS   4096
#define DYN_URING_BUFSZ   2048
#define DYN_URING_BGID    1
#define DYN_URING_ACCEPT_UD 0ULL

enum { DYN_OP_ACCEPT = 0, DYN_OP_RECV = 1, DYN_OP_SEND = 2 };
#define DYN_UD_TAG(ud)  ((int)((ud) & 7))
#define DYN_UD_CONN(ud) ((dyn_uconn_t *)(uintptr_t)((ud) & ~(uint64_t)7))

/* One connection in the completion model. `refs` are the outstanding io_uring
 * ops that carry this pointer as user_data (a live multishot recv + an in-flight
 * send); the object is freed only once both are gone and it is closing. */
typedef struct dyn_uconn {
    int fd;
    dyn_bytes_t in;   /* accumulated request bytes (copied out of pool buffers) */
    dyn_bytes_t out;  /* response bytes pending send (immutable while sending) */
    size_t out_off;
    int nreq;
    unsigned recv_armed : 1;
    unsigned send_inflight : 1;
    unsigned closing : 1;
    struct dyn_uconn *next, *prev; /* server's live-connection list */
} dyn_uconn_t;

typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    unsigned char *buf_base; /* nbufs * bufsz slab backing the provided ring */
    int nbufs, bufsz;
    dyn_uconn_t *live;
} dyn_uring_ctx;

/* Get an SQE, flushing the backlog and retrying once if the SQ ring is full. */
static struct io_uring_sqe *dyn_ur_sqe(dyn_uring_ctx *u)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&u->ring);
    if (!sqe) {
        io_uring_submit(&u->ring);
        sqe = io_uring_get_sqe(&u->ring);
    }
    return sqe;
}

static int dyn_ur_arm_accept(dyn_uring_ctx *u, int listen_fd)
{
    struct io_uring_sqe *sqe = dyn_ur_sqe(u);
    if (!sqe)
        return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, DYN_URING_ACCEPT_UD);
    return 0;
}

static void dyn_ur_arm_recv(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    struct io_uring_sqe *sqe = dyn_ur_sqe(u);
    if (!sqe)
        return;
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = DYN_URING_BGID;
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_RECV);
    c->recv_armed = 1;
}

static void dyn_ur_submit_send(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    struct io_uring_sqe *sqe = dyn_ur_sqe(u);
    if (!sqe)
        return;
    io_uring_prep_send(sqe, c->fd, c->out.data + c->out_off,
                       c->out.len - c->out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_SEND);
    c->send_inflight = 1;
}

static void dyn_ur_recycle(dyn_uring_ctx *u, int bid)
{
    io_uring_buf_ring_add(u->br, u->buf_base + (size_t)bid * u->bufsz, u->bufsz,
                          bid, io_uring_buf_ring_mask(u->nbufs), 0);
    io_uring_buf_ring_advance(u->br, 1);
}

static dyn_uconn_t *dyn_uconn_new(dyn_uring_ctx *u, int fd)
{
    dyn_uconn_t *c = (dyn_uconn_t *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->next = u->live;
    if (u->live)
        u->live->prev = c;
    u->live = c;
    return c;
}

static void dyn_uconn_destroy(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        u->live = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (c->fd >= 0)
        close(c->fd);
    free(c->in.data);
    free(c->out.data);
    free(c);
}

/* Progress a closing connection: once no send is in flight, drop the fd (which
 * terminates the multishot recv with a final CQE), then free when no op remains
 * that still references this object. */
static void dyn_uconn_close_step(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    if (!c->closing || c->send_inflight)
        return;
    if (c->recv_armed) {
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1; /* terminal recv CQE will clear recv_armed */
        }
        return;
    }
    dyn_uconn_destroy(u, c);
}

static void dyn_ur_on_accept(dyn_uring_ctx *u, dyn_http_async_t *s,
                             struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE)) /* multishot accept ended: re-arm */
        dyn_ur_arm_accept(u, s->listen_fd);
    if (res < 0)
        return; /* transient accept error; the re-arm above keeps us listening */
    {
        dyn_uconn_t *c = dyn_uconn_new(u, res);
        if (!c) {
            close(res);
            return;
        }
        dyn_set_nodelay(res);
        dyn_ur_arm_recv(u, c);
    }
}

static void dyn_ur_on_recv(dyn_uring_ctx *u, dyn_http_async_t *s,
                           dyn_uconn_t *c, struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    int bid = (cqe->flags & IORING_CQE_F_BUFFER)
                  ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;

    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->recv_armed = 0; /* multishot recv terminated */

    if (res > 0) {
        int oom = 0;
        if (bid >= 0) {
            if (dyn_bytes_append(&c->in,
                                 (char *)(u->buf_base + (size_t)bid * u->bufsz),
                                 (size_t)res) < 0)
                oom = 1;
            dyn_ur_recycle(u, bid);
        }
        if (oom || c->in.len > DYN_ACONN_MAX_REQ) {
            c->closing = 1;
        } else if (!c->send_inflight) {
            /* only touch `out` when no send references it (no realloc-under-send) */
            if (dyn_http_pump(&c->in, &c->out, &c->nreq, s->routes, s->n_routes))
                c->closing = 1;
            if (c->out.len > c->out_off)
                dyn_ur_submit_send(u, c);
        }
        if (!c->closing && !c->recv_armed)
            dyn_ur_arm_recv(u, c);
    } else {
        if (bid >= 0)
            dyn_ur_recycle(u, bid);
        if (res == -ENOBUFS) {
            if (!c->closing && !c->recv_armed) /* pool momentarily drained */
                dyn_ur_arm_recv(u, c);
        } else { /* EOF (0) or hard error: flush what we have, then close */
            if (!c->send_inflight) {
                dyn_http_pump(&c->in, &c->out, &c->nreq, s->routes, s->n_routes);
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
            }
            c->closing = 1;
        }
    }
    dyn_uconn_close_step(u, c);
}

static void dyn_ur_on_send(dyn_uring_ctx *u, dyn_http_async_t *s,
                           dyn_uconn_t *c, struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    c->send_inflight = 0;
    if (res > 0) {
        c->out_off += (size_t)res;
        if (c->out_off < c->out.len) {
            dyn_ur_submit_send(u, c); /* short send: ship the remainder */
        } else {
            c->out.len = 0;
            c->out_off = 0;
            if (!c->closing) {
                /* response flushed; drain any pipelined requests buffered while
                 * the send held `out` */
                if (dyn_http_pump(&c->in, &c->out, &c->nreq, s->routes,
                                  s->n_routes))
                    c->closing = 1;
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
                else if (!c->recv_armed && !c->closing)
                    dyn_ur_arm_recv(u, c);
            }
        }
    } else {
        c->closing = 1;
    }
    dyn_uconn_close_step(u, c);
}

/* Returns 1 if the io_uring reactor initialised and ran (and cleaned up); 0 if
 * init failed (caller falls back to the readiness reactor). */
static int dyn_http_uring_try_run(dyn_http_async_t *s)
{
    dyn_uring_ctx *u = (dyn_uring_ctx *)calloc(1, sizeof(*u));
    struct io_uring_params p;
    int ret = 0, i;

    if (!u)
        return 0;
    u->nbufs = DYN_URING_NBUFS;
    u->bufsz = DYN_URING_BUFSZ;

    /* Prefer the modern single-issuer/deferred-taskrun setup; degrade on older
     * kernels that reject the flags. */
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
        memset(&p, 0, sizeof(p));
        p.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
            memset(&p, 0, sizeof(p));
            if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
                free(u);
                return 0;
            }
        }
    }

    u->buf_base = (unsigned char *)malloc((size_t)u->nbufs * u->bufsz);
    if (!u->buf_base) {
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    u->br = io_uring_setup_buf_ring(&u->ring, u->nbufs, DYN_URING_BGID, 0, &ret);
    if (!u->br) {
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    for (i = 0; i < u->nbufs; i++)
        io_uring_buf_ring_add(u->br, u->buf_base + (size_t)i * u->bufsz, u->bufsz,
                              i, io_uring_buf_ring_mask(u->nbufs), i);
    io_uring_buf_ring_advance(u->br, u->nbufs);

    if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
        io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    io_uring_submit(&u->ring);

    s->uring = u;
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 200000000L};
        struct io_uring_cqe *cqe;
        unsigned head, count = 0;

        ret = io_uring_submit_and_wait_timeout(&u->ring, &cqe, 1, &ts, NULL);
        if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -ETIMEDOUT)
            break;
        io_uring_for_each_cqe(&u->ring, head, cqe) {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            count++;
            if (ud == DYN_URING_ACCEPT_UD) {
                dyn_ur_on_accept(u, s, cqe);
            } else {
                dyn_uconn_t *c = DYN_UD_CONN(ud);
                if (DYN_UD_TAG(ud) == DYN_OP_RECV)
                    dyn_ur_on_recv(u, s, c, cqe);
                else if (DYN_UD_TAG(ud) == DYN_OP_SEND)
                    dyn_ur_on_send(u, s, c, cqe);
            }
        }
        io_uring_cq_advance(&u->ring, count);
    }

    while (u->live)
        dyn_uconn_destroy(u, u->live);
    io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
    free(u->buf_base);
    io_uring_queue_exit(&u->ring);
    free(u);
    s->uring = NULL;
    return 1;
}
#endif /* DYN_HTTP_HAVE_URING */

static void *dyn_http_async_reactor(void *arg)
{
    dyn_http_async_t *s = (dyn_http_async_t *)arg;

#ifdef DYN_HTTP_HAVE_URING
    if (dyn_http_uring_try_run(s))
        return NULL; /* the io_uring reactor ran to shutdown and cleaned up */
    /* io_uring init failed on this kernel: fall back to the readiness reactor */
#endif

    s->loop = dyn_evloop_new();
    if (!s->loop ||
        dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ, dyn_alisten_cb, s) < 0) {
        atomic_store_explicit(&s->spawn_ok, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        if (dyn_evloop_poll(s->loop, 200) < 0) /* tick to observe stop_flag */
            break;
    }
    return NULL;
}

static void dyn_http_async_stop_internal(dyn_http_async_t *s)
{
    if (!s->started)
        return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    pthread_join(s->reactor, NULL);
    if (s->loop) {
        dyn_evloop_free(s->loop); /* reactor is joined: sole owner now */
        s->loop = NULL;
    }
    s->started = 0;
}

static void dyn_http_async_dispose(void *native)
{
    dyn_http_async_t *s = (dyn_http_async_t *)native;
    size_t i;

    dyn_http_async_stop_internal(s);
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
    free(s);
}

static JSValue dyn_http_async_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    dyn_http_async_t *s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char *host_c = NULL;
    char *host_dup = NULL;
    int32_t port = 0, backlog = 0;
    JSPropertyEnum *tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    (void)new_target;
    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
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
        routes_val = JS_GetPropertyStr(ctx, opts, "routes");
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
    if (backlog <= 0)
        backlog = SOMAXCONN;

    s = (dyn_http_async_t *)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->backlog = backlog;
    atomic_init(&s->stop_flag, 0);
    atomic_init(&s->spawn_ok, 0);

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

    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind(host_dup, &bound_port, backlog);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL);
    }
    if (dyn_set_nonblock(listen_fd) < 0) {
        close(listen_fd);
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, dyn_http_async_class_id, s,
                        dyn_http_async_dispose);

 oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_ThrowOutOfMemory(ctx);

 fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
 fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_EXCEPTION;
}

static JSValue dyn_http_async_start(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    atomic_store_explicit(&s->spawn_ok, 0, memory_order_relaxed);
    if (pthread_create(&s->reactor, NULL, dyn_http_async_reactor, s) != 0)
        return JS_ThrowInternalError(ctx, "failed to start reactor thread");
    /* wait for the reactor to publish loop-init success/failure */
    for (;;) {
        int ok = atomic_load_explicit(&s->spawn_ok, memory_order_acquire);
        if (ok != 0) {
            if (ok < 0) {
                pthread_join(s->reactor, NULL);
                return JS_ThrowInternalError(ctx, "reactor failed to init loop");
            }
            break;
        }
        sched_yield();
    }
    s->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_stop(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_async_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_http_async_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_async_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_async_stop),
    JS_CGETSET_DEF("port", dyn_http_async_get_port, NULL),
};

static const JSClassDef dyn_http_async_class = {
    "HttpServerAsync",
    .finalizer = dyn_res_finalizer,
};

/* ==================================================================== *
 *  App -- the controlled JS-handler server (Model B)                    *
 *                                                                        *
 *  Runs ENTIRELY on the JS thread via the dyn_aio reactor folded into   *
 *  js_std_loop -- so handlers are plain JS, invoked with a zero-copy     *
 *  view of the request, no cross-thread hop. Low-level HTTP is not       *
 *  exposed; a user registers typed routes: .rpc (strict JSON-RPC 2.0),  *
 *  and (next) .static/.upload/.ws. This is the usable-server fix.        *
 * ==================================================================== */

enum { APP_RPC = 1, APP_STATIC, APP_UPLOAD, APP_WS };

typedef struct {
    char *path;       /* exact match (rpc/upload/ws) or prefix (static) */
    int type;
    JSValue handler;  /* rpc: methods object; upload/ws: handler(s) */
    char *dir;        /* static/upload: filesystem root */
    int64_t max_file; /* static/upload: size cap (0 = default) */
    char **allow;     /* file-type filter: NULL=allow all; else ".ext"/"mime" list */
    size_t n_allow;
} dyn_app_route_t;

typedef struct dyn_app {
    JSContext *ctx;
    dyn_aio_t *aio;
    int listen_fd;
    uint16_t port;
    dyn_app_route_t *routes;
    size_t n_routes;
    int started;
} dyn_app_t;

typedef struct dyn_ws dyn_ws_t;
typedef struct dyn_app_upload dyn_app_upload_t;

typedef struct {
    dyn_app_t *app;
    int fd;
    dyn_iobuf_t in; /* accumulated request bytes (keep-alive/pipelined) */
    dyn_app_upload_t *up; /* non-NULL while streaming a large upload to disk */
    int refs;       /* 1 for the live connection + 1 per in-flight async handler */
    int closed;     /* peer gone: settle callbacks must not send, just unref */
    /* WebSocket mode (after a successful Upgrade on a ws route) */
    int is_ws;
    JSValue ws_handlers; /* {open,message,close} (dup) */
    JSValue ws_this;     /* the WsConn JS object (conn holds a strong ref) */
    dyn_ws_t *ws_native; /* WsConn opaque; ws_native->conn nulled on teardown */
    dyn_iobuf_t ws_frag; /* reassembly buffer for a fragmented message */
    int ws_frag_op;      /* opcode of the in-progress fragmented message, or 0 */
} dyn_app_conn_t;

struct dyn_app_upload {
    int fd;
    int64_t remaining; /* body bytes not yet written */
    int64_t size;      /* total content-length */
    char *path;
    char ctype[128];
    const dyn_app_route_t *route;
};

static void dyn_app_conn_unref(dyn_app_conn_t *c)
{
    if (--c->refs == 0) {
        if (c->up) { /* connection died mid-upload: drop the partial file */
            close(c->up->fd);
            unlink(c->up->path);
            free(c->up->path);
            free(c->up);
        }
        JS_FreeValue(c->app->ctx, c->ws_handlers);
        JS_FreeValue(c->app->ctx, c->ws_this);
        dyn_iobuf_free(&c->ws_frag);
        dyn_iobuf_free(&c->in);
        free(c);
    }
}

static void dyn_app_conn_close(dyn_app_conn_t *c);

/* ---- WebSocket (RFC 6455) ------------------------------------------- */

struct dyn_ws { dyn_app_conn_t *conn; }; /* nulled on teardown; borrowed ptr */
static JSClassID dyn_ws_class_id;

/* compact one-shot SHA-1 (handshake accept only) */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t b[64]; size_t n; } dws_sha1;
static void dws_sha1_blk(dws_sha1 *c, const uint8_t *p)
{
    uint32_t w[80], a, b, cc, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (i = 16; i < 80; i++) { uint32_t x = w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=(x<<1)|(x>>31); }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4];
    for (i = 0; i < 80; i++) {
        if (i<20){f=(b&cc)|(~b&d);k=0x5A827999;}
        else if(i<40){f=b^cc^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&cc)|(b&d)|(cc&d);k=0x8F1BBCDC;}
        else {f=b^cc^d;k=0xCA62C1D6;}
        t=((a<<5)|(a>>27))+f+e+k+w[i]; e=d; d=cc; cc=(b<<30)|(b>>2); b=a; a=t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e;
}
static void dws_sha1_run(const uint8_t *msg, size_t mlen, uint8_t out[20])
{
    dws_sha1 c; size_t i; uint64_t bits;
    c.h[0]=0x67452301; c.h[1]=0xEFCDAB89; c.h[2]=0x98BADCFE; c.h[3]=0x10325476;
    c.h[4]=0xC3D2E1F0; c.len=mlen; c.n=0;
    for (i = 0; i + 64 <= mlen; i += 64) dws_sha1_blk(&c, msg + i);
    { uint8_t tail[128]; size_t r = mlen - i, tn;
      memcpy(tail, msg + i, r); tail[r] = 0x80; tn = r + 1;
      while ((tn % 64) != 56) tail[tn++] = 0;
      bits = mlen * 8;
      { int j; for (j = 0; j < 8; j++) tail[tn+j] = (uint8_t)(bits >> ((7-j)*8)); }
      tn += 8;
      for (i = 0; i < tn; i += 64) dws_sha1_blk(&c, tail + i); }
    { int j; for (j = 0; j < 5; j++) {
        out[j*4]=(c.h[j]>>24)&0xff; out[j*4+1]=(c.h[j]>>16)&0xff;
        out[j*4+2]=(c.h[j]>>8)&0xff; out[j*4+3]=c.h[j]&0xff; } }
}
static void dws_b64(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i]<<16)|(in[i+1]<<8)|in[i+2];
        out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63];
        out[o++]=T[(v>>6)&63]; out[o++]=T[v&63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i]<<16; if (i+1<n) v |= (uint32_t)in[i+1]<<8;
        out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63];
        out[o++]=(i+1<n)?T[(v>>6)&63]:'='; out[o++]='=';
    }
    out[o]=0;
}
static void dws_accept(const char *key, size_t klen, char out[32])
{
    uint8_t buf[64], d[20];
    size_t kn = klen < 24 ? klen : 24;
    memcpy(buf, key, kn);
    memcpy(buf + kn, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    dws_sha1_run(buf, kn + 36, d);
    dws_b64(d, 20, out);
}

/* Send a server frame (FIN=1, unmasked). opcode: 1 text, 2 binary, 8 close,
 * 9 ping, 10 pong. */
static void dyn_ws_send_frame(dyn_app_conn_t *c, int opcode, const uint8_t *data,
                              size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10];
    size_t hn;
    int i;
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) { h[1] = (uint8_t)len; hn = 2; }
    else if (len <= 0xffff) { h[1]=126; h[2]=(len>>8)&0xff; h[3]=len&0xff; hn=4; }
    else { h[1]=127; for (i=0;i<8;i++) h[2+i]=(uint8_t)((uint64_t)len>>((7-i)*8)); hn=10; }
    dyn_iobuf_append(&f, h, hn);
    if (len) dyn_iobuf_append(&f, data, len);
    dyn_aio_send(c->app->aio, c->fd, f.data, f.len, 0, NULL, NULL);
    dyn_iobuf_free(&f);
}

static void dyn_ws_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(val, dyn_ws_class_id);
    (void)rt;
    free(w);
}
static const JSClassDef dyn_ws_class = { "WsConn", .finalizer = dyn_ws_finalizer };

/* conn.send(data) -- data string => text frame, ArrayBuffer => binary. */
static JSValue dyn_ws_send(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(this_val, dyn_ws_class_id);
    const char *str = NULL;
    uint8_t *abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w) return JS_ThrowTypeError(ctx, "not a WsConn");
    if (argc < 1) return JS_UNDEFINED;
    /* coerce data FIRST (may run JS), THEN check the (possibly closed) conn */
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) return JS_EXCEPTION;
    } else {
        abuf = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (abuf) binary = 1;
        else { str = JS_ToCStringLen(ctx, &len, argv[0]); if (!str) return JS_EXCEPTION; }
    }
    if (w->conn && !w->conn->closed)
        dyn_ws_send_frame(w->conn, binary ? 2 : 1,
                          str ? (const uint8_t *)str : abuf, len);
    if (str) JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue dyn_ws_close_method(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(this_val, dyn_ws_class_id);
    (void)argc; (void)argv;
    if (!w) return JS_ThrowTypeError(ctx, "not a WsConn");
    if (w->conn && !w->conn->closed) {
        dyn_ws_send_frame(w->conn, 8, NULL, 0); /* close frame */
        dyn_app_conn_close(w->conn);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_ws_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_ws_send),
    JS_CFUNC_DEF("close", 0, dyn_ws_close_method),
};

/* Perform the RFC 6455 Upgrade on connection `c`: send 101, switch to WS mode,
 * create the WsConn object, call the open handler. Returns 1 if upgraded. */
static int dyn_app_ws_handshake(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                const char *base, size_t head_len)
{
    JSContext *ctx = c->app->ctx;
    const char *key, *upg;
    size_t keylen = 0, upglen = 0;
    char accept[32], resp[256];
    int rn;
    dyn_ws_t *w;
    JSValue obj, oh;

    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    upg = dyn_req_header(base, head_len, "upgrade", &upglen);
    if (!key || keylen == 0 || !upg)
        return 0;
    dws_accept(key, keylen, accept);
    rn = snprintf(resp, sizeof(resp),
                  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    dyn_aio_send(c->app->aio, c->fd, resp, (size_t)rn, 0, NULL, NULL);

    c->is_ws = 1;
    c->ws_handlers = JS_DupValue(ctx, rt->handler);
    w = (dyn_ws_t *)malloc(sizeof(*w));
    if (!w)
        return 1; /* upgraded; capability object unavailable (OOM) */
    w->conn = c;
    c->ws_native = w;
    obj = JS_NewObjectClass(ctx, dyn_ws_class_id);
    JS_SetOpaque(obj, w);
    c->ws_this = obj; /* the connection owns this reference */

    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        JSValue r = JS_Call(ctx, oh, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, oh);
    return 1;
}

static void dyn_app_ws_dispatch_msg(dyn_app_conn_t *c, int opcode,
                                    const uint8_t *payload, size_t plen)
{
    JSContext *ctx = c->app->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, c->ws_handlers, "message");
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? JS_NewArrayBufferCopy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char *)payload, plen);
        JSValueConst args[3] = { c->ws_this, data, JS_NewBool(ctx, opcode == 2) };
        JSValue r = JS_Call(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

/* Parse and dispatch every complete WS frame buffered in c->in. Caller holds a
 * ref on `c` (a control frame may close it mid-loop). v1: FIN assumed (each
 * frame is a complete message); client frames must be masked (per RFC). */
static void dyn_app_ws_process(dyn_app_conn_t *c)
{
    for (;;) {
        uint8_t *p = dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        int opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t *mask, *payload;

        int fin;
        if (avail < 2)
            return;
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        plen = p[1] & 0x7f;
        hdr = 2;
        if (plen == 126) {
            if (avail < 4) return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10) return;
            plen = 0;
            for (i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (!masked) { dyn_app_conn_close(c); return; } /* RFC: must be masked */
        if (plen > DYN_ACONN_MAX_REQ) { /* cap BEFORE the size math to avoid
                                         * hdr+4+plen overflow -> OOB read */
            dyn_app_conn_close(c);
            return;
        }
        if (avail < hdr + 4 + plen) return; /* need mask key + full payload */
        mask = p + hdr;
        payload = p + hdr + 4;
        for (i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
        frame_total = hdr + 4 + (size_t)plen;

        if (opcode == 0x8) {                /* close */
            dyn_ws_send_frame(c, 8, NULL, 0);
            dyn_iobuf_consume(&c->in, frame_total);
            dyn_app_conn_close(c);
            return;
        } else if (opcode == 0x9) {         /* ping -> pong */
            dyn_ws_send_frame(c, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {         /* pong: ignore */
        } else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
            int frag_active = c->ws_frag_op != 0;
            /* protocol errors: stray continuation, or a new message mid-fragment */
            if ((opcode == 0x0 && !frag_active) || (opcode != 0x0 && frag_active)) {
                dyn_app_conn_close(c); return;
            }
            if (opcode != 0x0 && fin) {
                dyn_app_ws_dispatch_msg(c, opcode, payload, (size_t)plen); /* unfragmented */
            } else { /* fragmented: accumulate, dispatch on FIN */
                if (opcode != 0x0) c->ws_frag_op = opcode;
                if (dyn_iobuf_rlen(&c->ws_frag) + plen > DYN_ACONN_MAX_REQ ||
                    dyn_iobuf_append(&c->ws_frag, payload, (size_t)plen) < 0) {
                    dyn_app_conn_close(c); return;
                }
                if (fin) {
                    dyn_app_ws_dispatch_msg(c, c->ws_frag_op,
                                            dyn_iobuf_rdata(&c->ws_frag),
                                            dyn_iobuf_rlen(&c->ws_frag));
                    dyn_iobuf_reset(&c->ws_frag);
                    c->ws_frag_op = 0;
                }
            }
        } /* other opcodes: ignore */
        dyn_iobuf_consume(&c->in, frame_total);
        if (c->closed)
            return;
    }
}

static JSClassID dyn_app_class_id;

/* Queue a full HTTP response (JSON body) onto the connection. */
static void dyn_app_send_json(dyn_app_conn_t *c, int status, const char *body,
                              size_t body_len)
{
    dyn_iobuf_t out;
    char head[256];
    int n;
    dyn_iobuf_init(&out);
    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                 status, dyn_reason_phrase(status), body_len);
    if (n > 0) {
        dyn_iobuf_append(&out, head, (size_t)n);
        if (body_len)
            dyn_iobuf_append(&out, body, body_len);
        dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0, NULL, NULL);
    }
    dyn_iobuf_free(&out);
}

/* Build a JSON-RPC 2.0 error string {"jsonrpc":"2.0","error":{code,message},id}
 * with a raw (already-JSON) id token, and send it. */
static void dyn_app_rpc_error(dyn_app_conn_t *c, int http_status, int code,
                              const char *msg, const char *id_json)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},"
                     "\"id\":%s}",
                     code, msg, id_json && *id_json ? id_json : "null");
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, (size_t)n);
}

/* Send a JSON-RPC success {"jsonrpc":"2.0","result":<result>,"id":<id>}. */
static void dyn_app_send_rpc_result(dyn_app_conn_t *c, JSValueConst result,
                                    const char *id_s)
{
    JSContext *ctx = c->app->ctx;
    size_t out_len;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    const char *out_s = JS_IsException(jstr) ? NULL
                                             : JS_ToCStringLen(ctx, &out_len, jstr);
    if (out_s) {
        dyn_iobuf_t o;
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(&o, out_s, out_len);
        dyn_iobuf_append(&o, ",\"id\":", 6);
        dyn_iobuf_append(&o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char *)o.data, o.len);
        dyn_iobuf_free(&o);
        JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr)) JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 500, -32603, "Internal error", id_s);
    }
    JS_FreeValue(ctx, jstr);
}

/* Send a JSON-RPC error from a thrown/rejected value. */
static void dyn_app_send_rpc_throw(dyn_app_conn_t *c, JSValueConst exc,
                                   const char *id_s)
{
    JSContext *ctx = c->app->ctx;
    const char *em = JS_ToCString(ctx, exc);
    dyn_app_rpc_error(c, 500, -32000, em ? em : "Server error", id_s);
    if (em) JS_FreeCString(ctx, em);
}

/* Carried by the promise settle closures (pointer smuggled as an int64 in the
 * function-data array; heap pointers fit in a double's 53-bit mantissa). */
typedef struct { dyn_app_conn_t *conn; char *id; } dyn_app_pending_t;

/* Promise reaction for an async rpc handler: magic 0 = fulfilled, 1 = rejected.
 * Runs on the JS thread when the handler's promise settles. */
static JSValue dyn_app_rpc_settle(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    int64_t ptr = 0;
    dyn_app_pending_t *pd;
    dyn_app_conn_t *c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    JS_ToInt64(ctx, &ptr, data[0]);
    pd = (dyn_app_pending_t *)(uintptr_t)ptr;
    c = pd->conn;
    if (!c->closed) {
        if (magic) dyn_app_send_rpc_throw(c, v, pd->id);
        else       dyn_app_send_rpc_result(c, v, pd->id);
    }
    free(pd->id);
    free(pd);
    dyn_app_conn_unref(c);
    return JS_UNDEFINED;
}

/* ---- JSON-RPC batch (array of requests) -- sync elements ------------- */

static void dyn_app_build_error(int code, const char *msg, const char *id_s,
                                dyn_iobuf_t *o)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},"
                     "\"id\":%s}", code, msg, id_s && *id_s ? id_s : "null");
    if (n > 0) dyn_iobuf_append(o, buf, (size_t)n);
}
static void dyn_app_build_result(JSContext *ctx, JSValueConst result,
                                 const char *id_s, dyn_iobuf_t *o)
{
    size_t out_len;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    const char *out_s = JS_IsException(jstr) ? NULL : JS_ToCStringLen(ctx, &out_len, jstr);
    if (out_s) {
        dyn_iobuf_append(o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(o, out_s, out_len);
        dyn_iobuf_append(o, ",\"id\":", 6);
        dyn_iobuf_append(o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(o, "}", 1);
        JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr)) JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_build_error(-32603, "Internal error", id_s, o);
    }
    JS_FreeValue(ctx, jstr);
}

/* Process one batch element into `out`. Returns 1 if a response was written (0
 * for a notification, i.e. no `id`). Sync handlers only; an async result in a
 * batch yields an error (single requests keep full async support). */
static int dyn_app_rpc_build_one(dyn_app_conn_t *c, JSValueConst methods,
                                 JSValueConst elem, dyn_iobuf_t *out, int comma)
{
    JSContext *ctx = c->app->ctx;
    JSValue method_v = JS_GetPropertyStr(ctx, elem, "method");
    JSValue id = JS_GetPropertyStr(ctx, elem, "id");
    JSValue params = JS_GetPropertyStr(ctx, elem, "params");
    int has_id = !JS_IsUndefined(id);
    const char *method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    const char *id_s;
    int wrote = 0;
    dyn_iobuf_t tmp;
    dyn_iobuf_init(&tmp);
    { JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
      id_s = JS_IsException(idj) ? NULL : JS_ToCString(ctx, idj);
      JS_FreeValue(ctx, idj); }

    if (!method_s) {
        if (has_id) { dyn_app_build_error(-32600, "Invalid Request", id_s, &tmp); wrote = 1; }
    } else {
        JSValue fn = JS_GetPropertyStr(ctx, methods, method_s);
        if (!JS_IsFunction(ctx, fn)) {
            if (has_id) { dyn_app_build_error(-32601, "Method not found", id_s, &tmp); wrote = 1; }
        } else {
            JSValueConst a[1] = { params };
            JSValue res = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
            if (JS_IsException(res)) {
                JSValue exc = JS_GetException(ctx);
                if (has_id) { const char *em = JS_ToCString(ctx, exc);
                    dyn_app_build_error(-32000, em ? em : "Server error", id_s, &tmp);
                    if (em) JS_FreeCString(ctx, em); wrote = 1; }
                JS_FreeValue(ctx, exc);
            } else {
                int thenable = 0;
                if (JS_IsObject(res)) {
                    JSValue th = JS_GetPropertyStr(ctx, res, "then");
                    thenable = JS_IsFunction(ctx, th);
                    JS_FreeValue(ctx, th);
                }
                if (thenable) {
                    if (has_id) { dyn_app_build_error(-32000, "async handler not allowed in batch", id_s, &tmp); wrote = 1; }
                } else if (has_id) {
                    dyn_app_build_result(ctx, res, id_s, &tmp); wrote = 1;
                }
                JS_FreeValue(ctx, res);
            }
        }
        JS_FreeValue(ctx, fn);
    }
    if (wrote) {
        if (comma) dyn_iobuf_append(out, ",", 1);
        dyn_iobuf_append(out, tmp.data, tmp.len);
    }
    dyn_iobuf_free(&tmp);
    if (method_s) JS_FreeCString(ctx, method_s);
    if (id_s) JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    return wrote;
}

static void dyn_app_dispatch_batch(dyn_app_conn_t *c, JSValueConst methods,
                                   JSValueConst arr)
{
    JSContext *ctx = c->app->ctx;
    uint32_t len = 0, i;
    int nresp = 0;
    dyn_iobuf_t out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    if (len == 0) { dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null"); return; }
    dyn_iobuf_init(&out);
    dyn_iobuf_append(&out, "[", 1);
    for (i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
        if (dyn_app_rpc_build_one(c, methods, elem, &out, nresp > 0))
            nresp++;
        JS_FreeValue(ctx, elem);
    }
    dyn_iobuf_append(&out, "]", 1);
    if (nresp > 0)
        dyn_app_send_json(c, 200, (const char *)out.data, out.len);
    else
        dyn_app_send_json(c, 200, "", 0); /* all notifications: no content */
    dyn_iobuf_free(&out);
}

/* Dispatch one complete JSON-RPC request body against a methods object. All JS
 * runs on this (the JS) thread. Sends the response on `c` (sync or, if the
 * handler returns a thenable, when the promise settles). */
static void dyn_app_dispatch_rpc(dyn_app_conn_t *c, JSValueConst methods,
                                 const char *body, size_t body_len)
{
    JSContext *ctx = c->app->ctx;
    JSValue req, method_v, params, id, fn, result;
    const char *method_s, *id_s;

    /* The JSON parser reads body[body_len] as a sentinel; the body is a slice of
     * our own mutable buffer, so NUL-terminate it in place (on_recv guarantees
     * the slot is writable) and restore the byte after. */
    { char *m = (char *)body;
      char save = m[body_len];
      m[body_len] = 0;
      req = JS_ParseJSON(ctx, body, body_len, "<rpc>");
      m[body_len] = save; }
    if (JS_IsException(req)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 400, -32700, "Parse error", "null");
        return;
    }
    if (JS_IsArray(ctx, req)) { /* JSON-RPC batch */
        dyn_app_dispatch_batch(c, methods, req);
        JS_FreeValue(ctx, req);
        return;
    }
    method_v = JS_GetPropertyStr(ctx, req, "method");
    id = JS_GetPropertyStr(ctx, req, "id");
    params = JS_GetPropertyStr(ctx, req, "params");
    /* serialize id back to a JSON token for the response */
    { JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
      id_s = JS_IsException(idj) ? NULL : JS_ToCString(ctx, idj);
      JS_FreeValue(ctx, idj); }

    method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    if (!method_s) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", id_s);
        goto cleanup;
    }
    fn = JS_GetPropertyStr(ctx, methods, method_s);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        dyn_app_rpc_error(c, 404, -32601, "Method not found", id_s);
        JS_FreeCString(ctx, method_s);
        goto cleanup;
    }
    /* call handler(params) */
    { JSValueConst argv[1] = { params };
      result = JS_Call(ctx, fn, JS_UNDEFINED, 1, argv); }
    JS_FreeValue(ctx, fn);
    JS_FreeCString(ctx, method_s);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        dyn_app_send_rpc_throw(c, exc, id_s);
        JS_FreeValue(ctx, exc);
        goto cleanup; /* result is JS_EXCEPTION: nothing to free */
    }
    /* async: handler returned a thenable -- settle the response later */
    if (JS_IsObject(result)) {
        JSValue then = JS_GetPropertyStr(ctx, result, "then");
        if (JS_IsFunction(ctx, then)) {
            dyn_app_pending_t *pd = (dyn_app_pending_t *)malloc(sizeof(*pd));
            if (pd) {
                JSValue dptr, onres, onrej, thenargs[2], tr;
                pd->conn = c;
                pd->id = strdup(id_s ? id_s : "null");
                c->refs++; /* keep the connection alive until it settles */
                dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)pd);
                onres = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 0, 1, &dptr);
                onrej = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 1, 1, &dptr);
                JS_FreeValue(ctx, dptr);
                thenargs[0] = onres;
                thenargs[1] = onrej;
                tr = JS_Call(ctx, then, result, 2, thenargs);
                JS_FreeValue(ctx, tr);
                JS_FreeValue(ctx, onres);
                JS_FreeValue(ctx, onrej);
                JS_FreeValue(ctx, then);
                JS_FreeValue(ctx, result);
                goto cleanup; /* response is sent when the promise settles */
            }
        }
        JS_FreeValue(ctx, then);
    }
    /* sync result */
    dyn_app_send_rpc_result(c, result, id_s);
    JS_FreeValue(ctx, result);
cleanup:
    if (id_s) JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, req);
}

static const char *dyn_app_content_type(const char *path)
{
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm")) return "text/html";
    if (!strcasecmp(d, ".js") || !strcasecmp(d, ".mjs")) return "text/javascript";
    if (!strcasecmp(d, ".css")) return "text/css";
    if (!strcasecmp(d, ".json")) return "application/json";
    if (!strcasecmp(d, ".txt")) return "text/plain";
    if (!strcasecmp(d, ".csv")) return "text/csv";
    if (!strcasecmp(d, ".png")) return "image/png";
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(d, ".gif")) return "image/gif";
    if (!strcasecmp(d, ".svg")) return "image/svg+xml";
    if (!strcasecmp(d, ".ico")) return "image/x-icon";
    if (!strcasecmp(d, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

/* File-type filter: 1 if `path` passes the route's allow-list (by ".ext" or
 * mime), or the route has no filter. */
static int dyn_app_allowed(const dyn_app_route_t *rt, const char *path)
{
    size_t k;
    const char *dot, *ct;
    if (!rt->allow) return 1;
    dot = strrchr(path, '.');
    ct = dyn_app_content_type(path);
    for (k = 0; k < rt->n_allow; k++) {
        if (dot && !strcasecmp(rt->allow[k], dot)) return 1;
        if (!strcmp(rt->allow[k], ct)) return 1;
    }
    return 0;
}

/* Send a small plain-text error as JSON. */
static void dyn_app_send_err(dyn_app_conn_t *c, int status, const char *msg)
{
    dyn_app_send_json(c, status, msg, strlen(msg));
}

/* Serve a file from a static route (blocking read + send; sendfile/async-disk
 * is the optimization). Path-traversal-proof, size-capped, type-filtered. */
static void dyn_app_serve_static(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                 const char *reqpath)
{
    const char *sub = reqpath + strlen(rt->path);
    char fpath[2048];
    struct stat st;
    int64_t cap;
    dyn_iobuf_t out;
    char head[512];
    int n, ffd;

    if (strstr(sub, "..")) { /* reject traversal outright */
        dyn_app_send_err(c, 403, "{\"error\":\"forbidden\"}");
        return;
    }
    while (*sub == '/') sub++;
    snprintf(fpath, sizeof(fpath), "%s/%s", rt->dir, *sub ? sub : "index.html");

    if (stat(fpath, &st) < 0 || !S_ISREG(st.st_mode)) {
        dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    cap = rt->max_file > 0 ? rt->max_file : (int64_t)(32 * 1024 * 1024);
    if ((int64_t)st.st_size > cap) {
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        return;
    }
    if (!dyn_app_allowed(rt, fpath)) {
        dyn_app_send_err(c, 403, "{\"error\":\"type not allowed\"}");
        return;
    }
    /* zero-copy: send the header, then sendfile the body straight from the page
     * cache (SIGBUS-safe, unlike mmap; the kernel handles a truncation). */
    ffd = open(fpath, O_RDONLY | O_CLOEXEC);
    if (ffd < 0) {
        dyn_app_send_err(c, 500, "{\"error\":\"open error\"}");
        return;
    }
    dyn_iobuf_init(&out);
    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                 "Connection: keep-alive\r\n\r\n",
                 dyn_app_content_type(fpath), (long long)st.st_size);
    if (n > 0) {
        dyn_iobuf_append(&out, head, (size_t)n);
        dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0, NULL, NULL);
    }
    dyn_iobuf_free(&out);
    /* dyn_aio_sendfile owns ffd and closes it on completion or connection close */
    dyn_aio_sendfile(c->app->aio, c->fd, ffd, 0, (size_t)st.st_size, NULL, NULL);
}

/* File upload: streamed straight to disk as the body arrives (no whole-body
 * buffering), so uploads are bounded by maxFileSize, not the request buffer. */
static unsigned long dyn_app_upload_seq;

/* Body fully written: close, invoke the handler with the saved path + metadata,
 * respond, and return the connection to HTTP mode. */
static void dyn_app_upload_finish(dyn_app_conn_t *c)
{
    dyn_app_upload_t *u = c->up;
    JSContext *ctx = c->app->ctx;
    JSValue pathv, meta, r;
    JSValueConst args[2];
    c->up = NULL; /* detach before running JS (handler must not re-enter it) */
    close(u->fd);
    pathv = JS_NewString(ctx, u->path);
    meta = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, meta, "size", JS_NewInt64(ctx, u->size));
    JS_SetPropertyStr(ctx, meta, "contentType", JS_NewString(ctx, u->ctype));
    args[0] = pathv; args[1] = meta;
    r = JS_Call(ctx, u->route->handler, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"handler failed\"}");
    } else {
        dyn_iobuf_t o;
        JSValue pj = JS_JSONStringify(ctx, pathv, JS_UNDEFINED, JS_UNDEFINED);
        const char *ps = JS_IsException(pj) ? NULL : JS_ToCString(ctx, pj);
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"ok\":true,\"path\":", 18);
        dyn_iobuf_append(&o, ps ? ps : "\"\"", strlen(ps ? ps : "\"\""));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char *)o.data, o.len);
        dyn_iobuf_free(&o);
        if (ps) JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, pj);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, pathv);
    JS_FreeValue(ctx, meta);
    free(u->path);
    free(u);
}

/* Write the body bytes currently buffered in c->in to the upload file; finish
 * when the declared content-length has been written. */
static void dyn_app_upload_drain(dyn_app_conn_t *c)
{
    dyn_app_upload_t *u = c->up;
    while (u->remaining > 0) {
        size_t take = dyn_iobuf_rlen(&c->in);
        ssize_t w;
        if (take == 0) return; /* need more bytes */
        if ((int64_t)take > u->remaining) take = (size_t)u->remaining;
        w = write(u->fd, dyn_iobuf_rdata(&c->in), take);
        if (w < 0) { if (errno == EINTR) continue; dyn_app_conn_close(c); return; }
        dyn_iobuf_consume(&c->in, (size_t)w);
        u->remaining -= w;
    }
    dyn_app_upload_finish(c);
}

/* Begin streaming an upload: validate size/type, open the dest file, consume the
 * header, write any buffered body, and switch the connection to upload mode. */
static void dyn_app_upload_start(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                 const char *base, size_t head_len, int64_t clen)
{
    const char *ct;
    size_t ctlen = 0;
    int64_t cap = rt->max_file > 0 ? rt->max_file : (int64_t)(16 * 1024 * 1024);
    char ctbuf[128], fpath[2048];
    dyn_app_upload_t *u;
    int fd;

    if (clen < 0 || clen > cap) {
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    ct = dyn_req_header(base, head_len, "content-type", &ctlen);
    { size_t n = ctlen < sizeof(ctbuf) - 1 ? ctlen : sizeof(ctbuf) - 1;
      char *sc;
      memcpy(ctbuf, ct ? ct : "", n); ctbuf[n] = 0;
      if ((sc = strchr(ctbuf, ';')) != NULL) *sc = 0;
      while (*ctbuf && ctbuf[strlen(ctbuf) - 1] == ' ') ctbuf[strlen(ctbuf) - 1] = 0; }
    if (rt->allow) {
        size_t k; int ok = 0;
        for (k = 0; k < rt->n_allow; k++)
            if (!strcmp(rt->allow[k], ctbuf)) { ok = 1; break; }
        if (!ok) { dyn_app_send_err(c, 403, "{\"error\":\"type not allowed\"}"); dyn_app_conn_close(c); return; }
    }
    snprintf(fpath, sizeof(fpath), "%s/up_%ld_%lu", rt->dir, (long)getpid(),
             ++dyn_app_upload_seq);
    fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { dyn_app_send_err(c, 500, "{\"error\":\"open failed\"}"); dyn_app_conn_close(c); return; }
    u = (dyn_app_upload_t *)calloc(1, sizeof(*u));
    if (!u) { close(fd); dyn_app_conn_close(c); return; }
    u->fd = fd; u->remaining = clen; u->size = clen; u->route = rt;
    u->path = strdup(fpath);
    memcpy(u->ctype, ctbuf, sizeof(u->ctype));
    c->up = u;
    dyn_iobuf_consume(&c->in, head_len); /* drop the request head */
    dyn_app_upload_drain(c);             /* write buffered body; finish if complete */
}

/* Route + dispatch every complete request buffered in c->in. */
static void dyn_app_process(dyn_app_conn_t *c)
{
    dyn_app_t *app = c->app;
    for (;;) {
        const char *base = (const char *)dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        const char *hdr_end = dyn_memfind(base, avail, "\r\n\r\n", 4);
        size_t head_len, body_len, req_total, clv_len = 0, i, r;
        const char *cl;
        char path[1024];
        int64_t clen = 0;
        const dyn_app_route_t *route = NULL;

        if (!hdr_end)
            return; /* need more header bytes */
        head_len = (size_t)(hdr_end - base) + 4;

        cl = dyn_req_header(base, head_len, "content-length", &clv_len);
        if (cl)
            for (i = 0; i < clv_len && cl[i] >= '0' && cl[i] <= '9'; i++) {
                clen = clen * 10 + (cl[i] - '0');
                if (clen > (int64_t)(1LL << 40)) { clen = (int64_t)(1LL << 40); break; }
            }

        if (dyn_parse_req_path(base, head_len, path, sizeof(path)) != 0) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad request\"}");
            dyn_app_conn_close(c);
            return;
        }
        for (r = 0; r < app->n_routes; r++) {
            const dyn_app_route_t *rt = &app->routes[r];
            if (rt->type == APP_STATIC) {
                if (strncmp(path, rt->path, strlen(rt->path)) == 0) { route = rt; break; }
            } else if (strcmp(rt->path, path) == 0) { route = rt; break; }
        }

        /* uploads stream to disk -- start before the whole body is buffered */
        if (route && route->type == APP_UPLOAD) {
            dyn_app_upload_start(c, route, base, head_len, clen);
            return;
        }

        /* other routes need the full body buffered (bodies over the cap never
         * complete and the connection is dropped by on_recv's MAX_REQ guard) */
        body_len = clen > DYN_ACONN_MAX_REQ ? (size_t)DYN_ACONN_MAX_REQ + 1 : (size_t)clen;
        req_total = head_len + body_len;
        if (avail < req_total)
            return;

        if (!route) {
            dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
        } else if (route->type == APP_RPC) {
            dyn_app_dispatch_rpc(c, route->handler, base + head_len, body_len);
        } else if (route->type == APP_STATIC) {
            dyn_app_serve_static(c, route, path);
        } else if (route->type == APP_WS) {
            if (dyn_app_ws_handshake(c, route, base, head_len)) {
                dyn_iobuf_consume(&c->in, req_total);
                return; /* upgraded: remaining bytes are WS frames */
            }
            dyn_app_send_err(c, 400, "{\"error\":\"expected websocket upgrade\"}");
        }
        dyn_iobuf_consume(&c->in, req_total);
        if (c->closed)
            return;
    }
}

static void dyn_app_conn_close(dyn_app_conn_t *c)
{
    if (c->closed)
        return;
    c->closed = 1;
    if (c->is_ws) {
        JSContext *ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->ws_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[3] = { c->ws_this, JS_NewInt32(ctx, 1000),
                                  JS_NewStringLen(ctx, "", 0) };
            JSValue r = JS_Call(ctx, ch, JS_UNDEFINED, 3, a);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a[2]);
        }
        JS_FreeValue(ctx, ch);
        if (c->ws_native)
            c->ws_native->conn = NULL; /* a retained WsConn now sends nothing */
    }
    dyn_aio_close(c->app->aio, c->fd);
    dyn_app_conn_unref(c); /* drop the connection's own ref (pending handlers keep it) */
}

static void dyn_app_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *ud)
{
    dyn_app_conn_t *c = (dyn_app_conn_t *)ud;
    (void)aio;
    if (res <= 0) { dyn_app_conn_close(c); return; }
    c->refs++; /* hold across processing: a handler/frame may close the conn */
    if (dyn_iobuf_append(&c->in, buf, len) < 0) {
        dyn_app_conn_close(c);
    } else if (!c->up && dyn_iobuf_rlen(&c->in) > DYN_ACONN_MAX_REQ) {
        dyn_app_conn_close(c); /* oversized non-streaming request */
    } else {
        if (c->up) {
            dyn_app_upload_drain(c); /* stream the upload body to disk */
        } else if (c->is_ws) {
            dyn_app_ws_process(c);
        } else {
            dyn_iobuf_ensure_nul(&c->in); /* writable sentinel slot at data[len] */
            dyn_app_process(c);           /* HTTP; may upgrade WS or start upload */
            if (!c->closed && c->is_ws)
                dyn_app_ws_process(c);    /* frames right after an upgrade */
        }
        if (!c->closed)
            dyn_iobuf_compact(&c->in); /* drop consumed prefix */
    }
    dyn_app_conn_unref(c);
}

static void dyn_app_on_accept(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *ud)
{
    dyn_app_t *app = (dyn_app_t *)ud;
    dyn_app_conn_t *c;
    (void)buf; (void)len;
    if (res < 0) return;
    c = (dyn_app_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_aio_close(aio, res); return; }
    c->app = app;
    c->fd = res;
    c->refs = 1; /* the live connection holds one ref */
    c->ws_handlers = JS_UNDEFINED;
    c->ws_this = JS_UNDEFINED;
    dyn_iobuf_init(&c->ws_frag);
    dyn_iobuf_init(&c->in);
    dyn_aio_recv(aio, res, 0, /*multishot=*/1, dyn_app_on_recv, c);
}

static void dyn_app_dispose(void *native)
{
    dyn_app_t *app = (dyn_app_t *)native;
    size_t i;
    if (app->aio) {
        if (app->started)
            js_std_set_io_reactor(app->ctx, -1, NULL, NULL);
        if (app->listen_fd >= 0)
            dyn_aio_close(app->aio, app->listen_fd);
        dyn_aio_free(app->aio);
    }
    for (i = 0; i < app->n_routes; i++) {
        size_t k;
        free(app->routes[i].path);
        free(app->routes[i].dir);
        for (k = 0; k < app->routes[i].n_allow; k++)
            free(app->routes[i].allow[k]);
        free(app->routes[i].allow);
        JS_FreeValue(app->ctx, app->routes[i].handler);
    }
    free(app->routes);
    free(app);
}

static const JSClassDef dyn_app_class = { "App", .finalizer = dyn_res_finalizer };

static JSValue dyn_app_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_app_t *app;
    int64_t port = 0;
    (void)new_target;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v)) JS_ToInt64(ctx, &port, v);
        JS_FreeValue(ctx, v);
    }
    app = (dyn_app_t *)calloc(1, sizeof(*app));
    if (!app) return JS_ThrowOutOfMemory(ctx);
    app->ctx = ctx;
    app->listen_fd = -1;
    app->port = (uint16_t)port;
    app->aio = dyn_aio_new(4096, 0);
    if (!app->aio) { free(app); return JS_ThrowOutOfMemory(ctx); }
    return dyn_res_wrap(ctx, dyn_app_class_id, app, dyn_app_dispose);
}

/* rpc(path, methodsObject) -- register a strict JSON-RPC 2.0 endpoint. */
static JSValue dyn_app_rpc(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path;
    dyn_app_route_t *nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "rpc(path, methods) requires a methods object");
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr)); /* realloc does not zero the new slot */
    nr->path = strdup(path);
    nr->type = APP_RPC;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* Coerce opts.allow (array of ".ext"/"mime" strings) into a fresh char** list. */
static char **dyn_app_parse_allow(JSContext *ctx, JSValueConst opts, size_t *pn)
{
    char **out = NULL;
    JSValue av = JS_GetPropertyStr(ctx, opts, "allow");
    *pn = 0;
    if (JS_IsArray(ctx, av)) {
        uint32_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, av, "length");
        JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        if (len)
            out = (char **)calloc(len, sizeof(char *));
        for (i = 0; out && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, av, i);
            const char *s = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (s) { out[*pn] = strdup(s); if (out[*pn]) (*pn)++; JS_FreeCString(ctx, s); }
        }
    }
    JS_FreeValue(ctx, av);
    return out;
}

/* static(prefix, dir[, {maxFileSize, allow}]) -- serve files from a directory. */
static JSValue dyn_app_static(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_app_t *app;
    const char *prefix, *dir;
    dyn_app_route_t *nr;
    char **allow = NULL;
    size_t n_allow = 0;
    int64_t maxf = 0;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "static(prefix, dir[, opts])");
    /* coerce ALL args to C locals BEFORE resolving the native handle */
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix) return JS_EXCEPTION;
    dir = JS_ToCString(ctx, argv[1]);
    if (!dir) { JS_FreeCString(ctx, prefix); return JS_EXCEPTION; }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxFileSize");
        if (!JS_IsUndefined(v)) JS_ToInt64(ctx, &maxf, v);
        JS_FreeValue(ctx, v);
        allow = dyn_app_parse_allow(ctx, argv[2], &n_allow);
    }

    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) goto fail;
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_ThrowOutOfMemory(ctx); goto fail; }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_STATIC;
    nr->handler = JS_UNDEFINED;
    nr->dir = strdup(dir);
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, prefix);
    JS_FreeCString(ctx, dir);
    return JS_UNDEFINED;
fail:
    { size_t k; for (k = 0; k < n_allow; k++) free(allow[k]); free(allow); }
    JS_FreeCString(ctx, prefix);
    JS_FreeCString(ctx, dir);
    return JS_EXCEPTION;
}

/* upload(path, {dir, maxFileSize, allow}, handler(savedPath, meta)). */
static JSValue dyn_app_upload(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path = NULL, *dir = NULL;
    dyn_app_route_t *nr;
    char **allow = NULL;
    size_t n_allow = 0, k;
    int64_t maxf = 0;
    JSValue handler = JS_UNDEFINED, v;

    if (argc < 3 || !JS_IsObject(argv[1]) || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "upload(path, {dir,...}, handler)");
    /* coerce ALL args to C locals BEFORE resolving the native handle */
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[1], "dir");
    dir = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!dir) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "upload: opts.dir required"); }
    v = JS_GetPropertyStr(ctx, argv[1], "maxFileSize");
    if (!JS_IsUndefined(v)) JS_ToInt64(ctx, &maxf, v);
    JS_FreeValue(ctx, v);
    allow = dyn_app_parse_allow(ctx, argv[1], &n_allow);
    handler = JS_DupValue(ctx, argv[2]);

    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) goto ufail;
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_ThrowOutOfMemory(ctx); goto ufail; }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_UPLOAD;
    nr->handler = handler;
    nr->dir = strdup(dir);
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, dir);
    return JS_UNDEFINED;
ufail:
    for (k = 0; k < n_allow; k++) free(allow[k]);
    free(allow);
    JS_FreeValue(ctx, handler);
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, dir);
    return JS_EXCEPTION;
}

/* ws(path, {open, message, close}) -- register a WebSocket endpoint. */
static JSValue dyn_app_ws_register(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path;
    dyn_app_route_t *nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "ws(path, {open,message,close})");
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_WS;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_app_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    (void)argc; (void)argv;
    if (!app) return JS_EXCEPTION;
    if (app->started) return JS_UNDEFINED;
    app->listen_fd = dyn_aio_listen(app->aio, "0.0.0.0", app->port, 1024);
    if (app->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "App: listen failed");
    dyn_aio_accept(app->aio, app->listen_fd, dyn_app_on_accept, app);
    js_std_set_io_reactor(ctx, dyn_aio_backend_fd(app->aio), dyn_aio_drain,
                          app->aio);
    app->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_app_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->port);
}

static const JSCFunctionListEntry dyn_app_proto[] = {
    JS_CFUNC_DEF("rpc", 2, dyn_app_rpc),
    JS_CFUNC_DEF("static", 2, dyn_app_static),
    JS_CFUNC_DEF("upload", 3, dyn_app_upload),
    JS_CFUNC_DEF("ws", 2, dyn_app_ws_register),
    JS_CFUNC_DEF("start", 0, dyn_app_start),
    JS_CGETSET_DEF("port", dyn_app_get_port, NULL),
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
    if (dyn_register_class(ctx, m, &dyn_http_async_class_id,
                           &dyn_http_async_class, dyn_http_async_proto,
                           countof(dyn_http_async_proto),
                           dyn_http_async_ctor, "HttpServerAsync") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_app_class_id,
                           &dyn_app_class, dyn_app_proto,
                           countof(dyn_app_proto),
                           dyn_app_ctor, "App") < 0)
        return -1;
    /* WsConn: internal class (created at handshake, not user-constructible) */
    JS_NewClassID(&dyn_ws_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), dyn_ws_class_id, &dyn_ws_class) < 0)
        return -1;
    {
        JSValue wp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, wp, dyn_ws_proto, countof(dyn_ws_proto));
        JS_SetClassProto(ctx, dyn_ws_class_id, wp);
    }
    return 0;
}

int js_nat_init_http(JSContext *ctx)
{
    JSModuleDef *m;
    /* Install the SIMD dispatch table on the JS thread before any acceptor/
     * worker thread spawns; dyn_memfind reads it lock-free thereafter. */
    simd_init();
    m = JS_NewCModule(ctx, "dyna:http", dyn_http_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "HttpClient");
    JS_AddModuleExport(ctx, m, "HttpServer");
    JS_AddModuleExport(ctx, m, "HttpServerAsync");
    JS_AddModuleExport(ctx, m, "App");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_HTTP */
