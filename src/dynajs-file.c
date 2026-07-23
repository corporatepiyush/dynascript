/*
 * dynajs:file -- buffered file reader/writer with a common JS API and per-platform
 * fast paths underneath (macOS and Linux).
 *
 *   import { FileReader, FileWriter, readFile, writeFile } from "dynajs:file";
 *
 *   const r = new FileReader(path, { bufferSize: 1<<18 });
 *   let line; while ((line = r.readLine()) !== null) { ... }
 *   r.close();
 *
 *   const w = new FileWriter(path, { append: false, preallocate: 1<<20 });
 *   w.write("hello\n"); w.write(anArrayBuffer);
 *   w.sync();   // durable (F_FULLFSYNC on macOS, fdatasync on Linux)
 *   w.close();
 *
 *   const text = readFile(path);          // one-shot, platform-optimal
 *   writeFile(path, text, { append:true });
 *
 * The JS surface is identical on every OS; the C layer picks the best primitive:
 *   Linux : posix_fadvise(SEQUENTIAL) on read, posix_fallocate for preallocate,
 *           fdatasync for durable flush, io_uring bulk read for readFile()
 *           (when CONFIG_IO_URING is compiled in).
 *   macOS : fcntl(F_RDAHEAD)+F_RDADVISE prefetch on read, F_PREALLOCATE for
 *           writes, F_FULLFSYNC for real durability (plain fsync does NOT flush
 *           to the platter on macOS).
 * Everything runs on the JS thread (no worker threads), but every method still
 * coerces its JS args to C locals BEFORE resolving the native handle, so a
 * valueOf/toString that closes `this` can never cause a use-after-free.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_FILE_DEFAULT_BUF (1u << 17) /* 128 KiB */
#define DYN_FILE_MIN_BUF     4096u
#define DYN_FILE_MAX_BUF     (1u << 26) /* 64 MiB cap on a caller-chosen size */

/* ==================================================================== *
 *  platform-specific primitives (the only OS #ifdefs in the module)     *
 * ==================================================================== */

/* Hint that the fd will be read sequentially and warm readahead. */
static void dyn_file_advise_seq_read(int fd, off_t size)
{
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    (void)size;
#elif defined(__APPLE__)
    fcntl(fd, F_RDAHEAD, 1);
    if (size > 0) {
        struct radvisory ra;
        ra.ra_offset = 0;
        ra.ra_count = size > INT_MAX ? INT_MAX : (int)size;
        fcntl(fd, F_RDADVISE, &ra); /* kick off an async prefetch */
    }
#else
    (void)fd;
    (void)size;
#endif
}

/* Best-effort reserve `size` bytes of backing store for a file being written
 * (reduces fragmentation and ENOSPC-mid-write). Returns 0. */
static int dyn_file_preallocate(int fd, off_t size)
{
    if (size <= 0)
        return 0;
#if defined(__linux__)
    /* Reserve backing blocks WITHOUT extending the logical file size
     * (posix_fallocate would zero-pad the file up to `size`). Best-effort:
     * some filesystems return EOPNOTSUPP. */
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
    fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, size);
    return 0;
#elif defined(__APPLE__)
    {
        fstore_t fst;
        fst.fst_flags = F_ALLOCATECONTIG;
        fst.fst_posmode = F_PEOFPOSMODE;
        fst.fst_offset = 0;
        fst.fst_length = size;
        fst.fst_bytesalloc = 0;
        if (fcntl(fd, F_PREALLOCATE, &fst) < 0) {
            fst.fst_flags = F_ALLOCATEALL; /* allow non-contiguous */
            fcntl(fd, F_PREALLOCATE, &fst);
        }
    }
    return 0;
#else
    (void)fd;
    return 0;
#endif
}

/* Durably flush written data to stable storage. */
static int dyn_file_durable_sync(int fd)
{
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0) /* the only true flush-to-platter on macOS */
        return 0;
    return fsync(fd); /* fall back if the fs doesn't support F_FULLFSYNC */
#elif defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

/* Read the whole file at `path` the fastest way this platform offers. Returns 0
 * (caller free()s *out) or -1. */
static int dyn_file_read_whole(const char *path, char **out, size_t *outlen)
{
#if defined(CONFIG_IO_URING) && defined(__linux__)
    return dyn_uring_read_all(path, out, outlen); /* high-QD io_uring path */
#else
    struct stat st;
    char *buf;
    size_t off = 0, size;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    dyn_file_advise_seq_read(fd, st.st_size);
    size = (size_t)st.st_size;
    buf = (char *)malloc(size ? size : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (off < size) {
        ssize_t r = read(fd, buf + off, size - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    *out = buf;
    *outlen = off;
    return 0;
#endif
}

static unsigned dyn_file_clamp_bufsize(int64_t v)
{
    if (v <= 0)
        return DYN_FILE_DEFAULT_BUF;
    if (v < DYN_FILE_MIN_BUF)
        return DYN_FILE_MIN_BUF;
    if (v > DYN_FILE_MAX_BUF)
        return DYN_FILE_MAX_BUF;
    return (unsigned)v;
}

/* ==================================================================== *
 *  FileReader -- buffered sequential reader                             *
 * ==================================================================== */

static JSClassID dyn_freader_class_id;

typedef struct {
    int fd;
    unsigned char *buf;
    size_t cap;
    size_t start; /* first unconsumed byte in buf */
    size_t end;   /* one past the last valid byte in buf */
    int eof;      /* underlying read returned 0 */
} dyn_freader_t;

static void dyn_freader_dispose(void *native)
{
    dyn_freader_t *r = (dyn_freader_t *)native;
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    free(r);
}

static const JSClassDef dyn_freader_class = {
    "FileReader",
    .finalizer = dyn_res_finalizer,
};

/* Refill buf from the fd when it is fully consumed. Returns bytes read, or -1. */
static ssize_t dyn_freader_fill(dyn_freader_t *r)
{
    ssize_t n;
    if (r->start < r->end)
        return (ssize_t)(r->end - r->start); /* still have buffered data */
    r->start = r->end = 0;
    if (r->eof)
        return 0;
    for (;;) {
        n = read(r->fd, r->buf, r->cap);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        break;
    }
    r->end = (size_t)n;
    if (n == 0)
        r->eof = 1;
    return n;
}

static JSValue dyn_freader_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_freader_t *r;
    const char *path;
    int64_t bufsize = 0;
    struct stat st;
    int fd;

    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileReader(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        JS_FreeCString(ctx, path);
        return JS_ThrowInternalError(ctx, "FileReader: cannot open file");
    }
    if (fstat(fd, &st) == 0)
        dyn_file_advise_seq_read(fd, st.st_size);
    JS_FreeCString(ctx, path);

    r = (dyn_freader_t *)calloc(1, sizeof(*r));
    if (!r) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->fd = fd;
    r->cap = dyn_file_clamp_bufsize(bufsize);
    r->buf = (unsigned char *)malloc(r->cap);
    if (!r->buf) {
        close(fd);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, dyn_freader_class_id, r, dyn_freader_dispose);
}

/* read([n]) -> up to n bytes as a string ("" at EOF); n omitted => read all. */
static JSValue dyn_freader_read(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_freader_t *r;
    int64_t want = -1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &want, argv[0])) /* coerce FIRST */
            return JS_EXCEPTION;
    }
    r = (dyn_freader_t *)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    {
        /* accumulate into a small growable buffer */
        char *acc = NULL;
        size_t acc_len = 0, acc_cap = 0;
        JSValue out;
        for (;;) {
            size_t avail, take;
            ssize_t f;
            if (want >= 0 && (int64_t)acc_len >= want)
                break;
            f = dyn_freader_fill(r);
            if (f < 0) {
                free(acc);
                return JS_ThrowInternalError(ctx, "FileReader: read error");
            }
            if (f == 0)
                break; /* EOF */
            avail = r->end - r->start;
            take = avail;
            if (want >= 0 && take > (size_t)(want - (int64_t)acc_len))
                take = (size_t)(want - (int64_t)acc_len);
            if (acc_len + take + 1 > acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8192;
                char *na;
                while (nc < acc_len + take + 1)
                    nc *= 2;
                na = (char *)realloc(acc, nc);
                if (!na) {
                    free(acc);
                    return JS_ThrowOutOfMemory(ctx);
                }
                acc = na;
                acc_cap = nc;
            }
            memcpy(acc + acc_len, r->buf + r->start, take);
            acc_len += take;
            r->start += take;
        }
        out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
        free(acc);
        return out;
    }
}

/* readLine() -> next line without the trailing newline, or null at EOF. */
static JSValue dyn_freader_read_line(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_freader_t *r;
    char *acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    int saw_any = 0;
    JSValue out;

    (void)argc; (void)argv;
    r = (dyn_freader_t *)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    for (;;) {
        ssize_t f = dyn_freader_fill(r);
        unsigned char *nl;
        size_t avail, take;
        if (f < 0) {
            free(acc);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break; /* EOF */
        saw_any = 1;
        avail = r->end - r->start;
        nl = (unsigned char *)memchr(r->buf + r->start, '\n', avail);
        take = nl ? (size_t)(nl - (r->buf + r->start)) : avail;
        if (acc_len + take + 1 > acc_cap) {
            size_t nc = acc_cap ? acc_cap * 2 : 256;
            char *na;
            while (nc < acc_len + take + 1)
                nc *= 2;
            na = (char *)realloc(acc, nc);
            if (!na) {
                free(acc);
                return JS_ThrowOutOfMemory(ctx);
            }
            acc = na;
            acc_cap = nc;
        }
        memcpy(acc + acc_len, r->buf + r->start, take);
        acc_len += take;
        r->start += take;
        if (nl) {
            r->start++; /* consume the '\n' */
            /* strip a trailing '\r' for CRLF files */
            if (acc_len > 0 && acc[acc_len - 1] == '\r')
                acc_len--;
            out = JS_NewStringLen(ctx, acc, acc_len);
            free(acc);
            return out;
        }
    }
    if (!saw_any && acc_len == 0) {
        free(acc);
        return JS_NULL; /* clean EOF with nothing buffered */
    }
    out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
    free(acc);
    return out;
}

static const JSCFunctionListEntry dyn_freader_proto[] = {
    JS_CFUNC_DEF("read", 0, dyn_freader_read),
    JS_CFUNC_DEF("readLine", 0, dyn_freader_read_line),
    JS_CFUNC_DEF("readAll", 0, dyn_freader_read),
};

/* ==================================================================== *
 *  FileWriter -- buffered writer                                        *
 * ==================================================================== */

static JSClassID dyn_fwriter_class_id;

typedef struct {
    int fd;
    unsigned char *buf;
    size_t cap;
    size_t len; /* buffered bytes not yet written to the fd */
} dyn_fwriter_t;

static void dyn_fwriter_flush_native(dyn_fwriter_t *w, int *err)
{
    size_t off = 0;
    *err = 0;
    while (off < w->len) {
        ssize_t n = write(w->fd, w->buf + off, w->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            *err = 1;
            return;
        }
        off += (size_t)n;
    }
    w->len = 0;
}

static void dyn_fwriter_dispose(void *native)
{
    dyn_fwriter_t *w = (dyn_fwriter_t *)native;
    int err;
    if (w->fd >= 0) {
        dyn_fwriter_flush_native(w, &err); /* best-effort flush on teardown */
        close(w->fd);
    }
    free(w->buf);
    free(w);
}

static const JSClassDef dyn_fwriter_class = {
    "FileWriter",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_fwriter_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_fwriter_t *w;
    const char *path;
    int64_t bufsize = 0, preallocate = 0;
    int append = 0, flags;
    int fd;

    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileWriter(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "preallocate");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt64(ctx, &preallocate, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "append");
        append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fd = open(path, flags, 0644);
    JS_FreeCString(ctx, path);
    if (fd < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: cannot open file");
    if (preallocate > 0)
        dyn_file_preallocate(fd, (off_t)preallocate);

    w = (dyn_fwriter_t *)calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->fd = fd;
    w->cap = dyn_file_clamp_bufsize(bufsize);
    w->buf = (unsigned char *)malloc(w->cap);
    if (!w->buf) {
        close(fd);
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, dyn_fwriter_class_id, w, dyn_fwriter_dispose);
}

/* Append `data`/`len` through the buffer, flushing when it fills; a write
 * larger than the buffer is sent directly after flushing what's buffered. */
static int dyn_fwriter_put(dyn_fwriter_t *w, const char *data, size_t len)
{
    int err;
    if (len >= w->cap) {
        size_t off = 0;
        dyn_fwriter_flush_native(w, &err);
        if (err)
            return -1;
        while (off < len) {
            ssize_t n = write(w->fd, data + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            off += (size_t)n;
        }
        return 0;
    }
    if (w->len + len > w->cap) {
        dyn_fwriter_flush_native(w, &err);
        if (err)
            return -1;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    return 0;
}

/* write(data): data is a string or an ArrayBuffer. Returns bytes accepted. */
static JSValue dyn_fwriter_write(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_fwriter_t *w;
    const char *str = NULL;
    uint8_t *abuf = NULL;
    size_t len = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "write(data) requires an argument");
    /* coerce the payload to a C view FIRST (may run user JS that closes this) */
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str)
            return JS_EXCEPTION;
    } else {
        abuf = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (!abuf) {
            /* fall back to string coercion for other types */
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
        }
    }

    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (dyn_fwriter_put(w, str ? str : (const char *)abuf, len) < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "FileWriter: write error");
    }
    if (str)
        JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx, (int64_t)len);
}

static JSValue dyn_fwriter_flush(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_fwriter_t *w;
    int err;
    (void)argc; (void)argv;
    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err)
        return JS_ThrowInternalError(ctx, "FileWriter: flush error");
    return JS_UNDEFINED;
}

static JSValue dyn_fwriter_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_fwriter_t *w;
    int err;
    (void)argc; (void)argv;
    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err || dyn_file_durable_sync(w->fd) < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: sync error");
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_fwriter_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_fwriter_write),
    JS_CFUNC_DEF("flush", 0, dyn_fwriter_flush),
    JS_CFUNC_DEF("sync", 0, dyn_fwriter_sync),
};

/* ==================================================================== *
 *  one-shot convenience functions                                       *
 * ==================================================================== */

static JSValue dyn_file_read_file(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    char *data = NULL;
    size_t len = 0;
    JSValue out;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (dyn_file_read_whole(path, &data, &len) < 0) {
        JS_FreeCString(ctx, path);
        return JS_ThrowInternalError(ctx, "readFile: cannot read file");
    }
    JS_FreeCString(ctx, path);
    out = JS_NewStringLen(ctx, data ? data : "", len);
    free(data);
    return out;
}

static JSValue dyn_file_write_file(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *path = NULL, *str = NULL;
    uint8_t *abuf = NULL;
    size_t len = 0, off = 0;
    int append = 0, flags, fd;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, options])");
    /* coerce everything first */
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        str = JS_ToCStringLen(ctx, &len, argv[1]);
    } else {
        abuf = JS_GetArrayBuffer(ctx, &len, argv[1]);
        if (!abuf)
            str = JS_ToCStringLen(ctx, &len, argv[1]);
    }
    if (!str && !abuf) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "append");
        append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fd = open(path, flags, 0644);
    JS_FreeCString(ctx, path);
    if (fd < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "writeFile: cannot open file");
    }
    {
        const char *src = str ? str : (const char *)abuf;
        int werr = 0;
        while (off < len) {
            ssize_t n = write(fd, src + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                werr = 1;
                break;
            }
            off += (size_t)n;
        }
        close(fd);
        if (str)
            JS_FreeCString(ctx, str);
        if (werr)
            return JS_ThrowInternalError(ctx, "writeFile: write error");
    }
    return JS_NewInt64(ctx, (int64_t)len);
}

static const JSCFunctionListEntry dyn_file_funcs[] = {
    JS_CFUNC_DEF("readFile", 1, dyn_file_read_file),
    JS_CFUNC_DEF("writeFile", 2, dyn_file_write_file),
};

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

static int dyn_file_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_freader_class_id, &dyn_freader_class,
                           dyn_freader_proto, countof(dyn_freader_proto),
                           dyn_freader_ctor, "FileReader") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_fwriter_class_id, &dyn_fwriter_class,
                           dyn_fwriter_proto, countof(dyn_fwriter_proto),
                           dyn_fwriter_ctor, "FileWriter") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_file_funcs,
                                  (int)countof(dyn_file_funcs));
}

int js_nat_init_file(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:file", dyn_file_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "FileReader");
    JS_AddModuleExport(ctx, m, "FileWriter");
    JS_AddModuleExportList(ctx, m, dyn_file_funcs, (int)countof(dyn_file_funcs));
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_FILE */
