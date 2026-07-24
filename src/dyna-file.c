/*
 * dyna:file -- filesystem module: buffered content I/O plus all filesystem
 * operations (metadata, directories, links, globbing, temp files). Common JS
 * API with per-platform fast paths underneath (macOS and Linux).
 *
 *   import { FileReader, FileWriter, readFile, writeFile,
 *            stat, lstat, exists, readDir, makeDir, remove, removeAll, rename,
 *            symlink, readLink, realPath, chmod, glob,
 *            tempDir, makeTempDir, makeTempFile } from "dyna:file";
 *
 * Buffered content I/O:
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
 *
 * Filesystem operations (transient plain functions, moved here from dyna:sys):
 *   Metadata : stat(path), lstat(path) -> { size, mode, isDir, isFile,
 *              isSymlink, mtimeMs, atimeMs, ctimeMs, uid, gid, ino, nlink };
 *              exists(path) -> bool (never throws).
 *   Dirs     : readDir(path) -> [{ name, isDir, isFile, isSymlink }] sorted by
 *              name (skips "."/".."); makeDir(path, {recursive, mode});
 *              remove(path); removeAll(path); rename(from, to).
 *   Links    : symlink(target, linkPath), readLink(path), realPath(path),
 *              chmod(path, mode).
 *   Glob     : glob(pattern, {cwd}) -> sorted, de-duplicated matches. Supports
 *              * (never crosses '/'), ** (spans directories), ? and [...]
 *              classes -- implemented here, no libc fnmatch/glob dependency.
 *   Temp     : tempDir(), makeTempDir(prefix), makeTempFile(prefix).
 * Guarantees: mode is the FULL st_mode (mask 0o777 for permissions); times are
 * epoch-milliseconds (sub-ms preserved); stat() follows symlinks, lstat() does
 * not; exists() uses lstat and never throws (true even for a dangling link);
 * removeAll() is recursive and symlink-safe (openat/unlinkat with O_NOFOLLOW,
 * depth-bounded); glob never follows a symlinked dir while expanding '**'.
 * These are plain functions (no `this`): each coerces every JS arg to an owned
 * C local, then performs the syscall, freeing on every path (incl. errors).
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_FILE_DEFAULT_BUF (1u << 17) /* 128 KiB */
#define DYN_FILE_MIN_BUF     4096u
#define DYN_FILE_MAX_BUF     (1u << 26) /* 64 MiB cap on a caller-chosen size */

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
        dyn_io_advise_seq_read(fd, st.st_size);
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
        dyn_io_preallocate(fd, (off_t)preallocate);

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
    if (err || dyn_io_durable_sync(w->fd) < 0)
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
    dyn_iobuf_t src;
    JSValue out;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    /* zero-copy mmap view for large files; a single copy into the JS string */
    if (dyn_io_slurp(path, &src, 0) < 0) {
        JS_FreeCString(ctx, path);
        return JS_ThrowInternalError(ctx, "readFile: cannot read file");
    }
    JS_FreeCString(ctx, path);
    out = JS_NewStringLen(ctx, (const char *)dyn_iobuf_rdata(&src),
                          dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
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

/* ==================================================================== *
 *  filesystem operations -- metadata, directories, links, globbing and
 *  temp files (moved here from dyna:sys; dyna:sys keeps process/env only).
 *  These are transient plain functions (no `this`): they coerce every JS
 *  arg to an owned C local, then perform the syscall, freeing on every path.
 * ==================================================================== */

/* Bounds on recursive filesystem walks (removeAll / glob). Real directory
 * trees are far shallower; these only guard against a pathological/adversarial
 * nesting so the JS-thread C stack can never overflow. */
#define DYN_FS_RMRF_MAX_DEPTH 512
#define DYN_FS_GLOB_MAX_DEPTH 512
#define DYN_FS_READLINK_MAX   (1u << 16)

static const char *dyn_fs_errno_code(int e)
{
    switch (e) {
    case ENOENT:        return "ENOENT";
    case EACCES:        return "EACCES";
    case EEXIST:        return "EEXIST";
    case ENOTDIR:       return "ENOTDIR";
    case EISDIR:        return "EISDIR";
    case ENOTEMPTY:     return "ENOTEMPTY";
    case EPERM:         return "EPERM";
    case ELOOP:         return "ELOOP";
    case ENAMETOOLONG:  return "ENAMETOOLONG";
    case EXDEV:         return "EXDEV";
    case EINVAL:        return "EINVAL";
    case ENOSPC:        return "ENOSPC";
    case EROFS:         return "EROFS";
    case EBUSY:         return "EBUSY";
    case EMFILE:        return "EMFILE";
    case ENFILE:        return "ENFILE";
    case ENOMEM:        return "ENOMEM";
    default:            return NULL;
    }
}

/* Build and throw a descriptive Error for a failed syscall. Returns
 * JS_EXCEPTION. `path` may be NULL. Reads errno via the `e` argument (captured
 * by the caller immediately after the failing call). */
static JSValue dyn_fs_throw(JSContext *ctx, int e, const char *op,
                             const char *path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char *code = dyn_fs_errno_code(e);

    if (path)
        snprintf(msg, sizeof(msg), "file.%s(\"%s\"): %s", op, path, strerror(e));
    else
        snprintf(msg, sizeof(msg), "file.%s: %s", op, strerror(e));

    err = JS_NewError(ctx);
    if (JS_IsException(err))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e),
                              JS_PROP_C_W_E);
    if (code)
        JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, code),
                                  JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

/* macOS names the sub-second stat fields differently from Linux. */
#if defined(__APPLE__)
#define DYN_STAT_MTIM(st) ((st).st_mtimespec)
#define DYN_STAT_ATIM(st) ((st).st_atimespec)
#define DYN_STAT_CTIM(st) ((st).st_ctimespec)
#else
#define DYN_STAT_MTIM(st) ((st).st_mtim)
#define DYN_STAT_ATIM(st) ((st).st_atim)
#define DYN_STAT_CTIM(st) ((st).st_ctim)
#endif

static double dyn_timespec_ms(struct timespec ts)
{
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* ==================================================================== *
 *  small path-string helpers (heap, sized to input)                     *
 * ==================================================================== */

/* Join two path fragments with a single '/'. If `a` is empty, returns a copy of
 * `b`; if `a` already ends with '/', no extra separator is inserted. Returns a
 * malloc'd NUL-terminated string, or NULL on OOM (caller frees). */
static char *dyn_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int sep = (la > 0 && a[la - 1] != '/');
    char *r;

    if (la == 0) {
        r = (char *)malloc(lb + 1);
        if (!r)
            return NULL;
        memcpy(r, b, lb + 1);
        return r;
    }
    r = (char *)malloc(la + (size_t)sep + lb + 1);
    if (!r)
        return NULL;
    memcpy(r, a, la);
    if (sep)
        r[la] = '/';
    memcpy(r + la + (size_t)sep, b, lb);
    r[la + (size_t)sep + lb] = '\0';
    return r;
}

static int dyn_is_dot_or_dotdot(const char *name)
{
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* ==================================================================== *
 *  stat / lstat / exists                                                *
 * ==================================================================== */

static JSValue dyn_fs_stat_common(JSContext *ctx, JSValueConst arg, int follow)
{
    const char *path;
    struct stat st;
    int r;
    JSValue obj;

    path = JS_ToCString(ctx, arg);
    if (!path)
        return JS_EXCEPTION;
    r = follow ? stat(path, &st) : lstat(path, &st);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, follow ? "stat" : "lstat", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, S_ISREG(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isSymlink",
                      JS_NewBool(ctx, S_ISLNK(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "mtimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_MTIM(st))));
    JS_SetPropertyStr(ctx, obj, "atimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_ATIM(st))));
    JS_SetPropertyStr(ctx, obj, "ctimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_CTIM(st))));
    JS_SetPropertyStr(ctx, obj, "uid", JS_NewInt32(ctx, (int32_t)st.st_uid));
    JS_SetPropertyStr(ctx, obj, "gid", JS_NewInt32(ctx, (int32_t)st.st_gid));
    JS_SetPropertyStr(ctx, obj, "ino", JS_NewInt64(ctx, (int64_t)st.st_ino));
    JS_SetPropertyStr(ctx, obj, "nlink",
                      JS_NewInt64(ctx, (int64_t)st.st_nlink));
    return obj;
}

static JSValue dyn_fs_stat(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 1);
}

static JSValue dyn_fs_lstat(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 0);
}

/* exists(path) -> bool. Never throws: any error (missing, permission, ...)
 * yields false. Uses lstat, so a dangling symlink reports true. */
static JSValue dyn_fs_exists(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *path;
    struct stat st;
    int ok;

    (void)this_val; (void)argc;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    ok = (lstat(path, &st) == 0);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

/* ==================================================================== *
 *  readDir                                                              *
 * ==================================================================== */

typedef struct {
    char *name;
    int is_dir, is_file, is_symlink;
} dyn_dirent_t;

static int dyn_dirent_cmp(const void *a, const void *b)
{
    const dyn_dirent_t *ea = (const dyn_dirent_t *)a;
    const dyn_dirent_t *eb = (const dyn_dirent_t *)b;
    return strcmp(ea->name, eb->name);
}

/* Fill (is_dir,is_file,is_symlink) for one entry. Uses readdir's d_type as a
 * fast path and falls back to lstat when it is unknown/unsupported. */
static void dyn_entry_type(int dtype, const char *fullpath, int *is_dir,
                           int *is_file, int *is_symlink)
{
    *is_dir = *is_file = *is_symlink = 0;
#ifdef DT_DIR
    switch (dtype) {
    case DT_DIR: *is_dir = 1; return;
    case DT_REG: *is_file = 1; return;
    case DT_LNK: *is_symlink = 1; return;
    default: break; /* DT_UNKNOWN or a type we don't surface -> lstat */
    }
#else
    (void)dtype;
#endif
    {
        struct stat st;
        if (lstat(fullpath, &st) == 0) {
            *is_dir = S_ISDIR(st.st_mode);
            *is_file = S_ISREG(st.st_mode);
            *is_symlink = S_ISLNK(st.st_mode);
        }
    }
}

static JSValue dyn_fs_read_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *path;
    size_t path_len;
    DIR *d;
    struct dirent *e;
    dyn_dirent_t *ents = NULL;
    size_t n = 0, cap = 0, i;
    char *fullbuf = NULL;
    size_t fullcap = 0;
    JSValue arr;
    int err = 0;

    (void)this_val; (void)argc;
    path = JS_ToCStringLen(ctx, &path_len, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    d = opendir(path);
    if (!d) {
        int en = errno;
        JSValue ex = dyn_fs_throw(ctx, en, "readDir", path);
        JS_FreeCString(ctx, path);
        return ex;
    }

    while ((e = readdir(d)) != NULL) {
        size_t nlen, need;
        int dtype;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        nlen = strlen(e->d_name);
        need = path_len + 1 + nlen + 1;
        if (need > fullcap) {
            char *nb = (char *)realloc(fullbuf, need);
            if (!nb) { err = 1; break; }
            fullbuf = nb;
            fullcap = need;
        }
        memcpy(fullbuf, path, path_len);
        fullbuf[path_len] = '/';
        memcpy(fullbuf + path_len + 1, e->d_name, nlen + 1);

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            dyn_dirent_t *ne =
                (dyn_dirent_t *)realloc(ents, ncap * sizeof(*ents));
            if (!ne) { err = 1; break; }
            ents = ne;
            cap = ncap;
        }
        ents[n].name = (char *)malloc(nlen + 1);
        if (!ents[n].name) { err = 1; break; }
        memcpy(ents[n].name, e->d_name, nlen + 1);
#ifdef DT_DIR
        dtype = e->d_type;
#else
        dtype = 0;
#endif
        dyn_entry_type(dtype, fullbuf, &ents[n].is_dir, &ents[n].is_file,
                       &ents[n].is_symlink);
        n++;
    }
    closedir(d);
    free(fullbuf);
    JS_FreeCString(ctx, path);

    if (err) {
        for (i = 0; i < n; i++)
            free(ents[i].name);
        free(ents);
        return JS_ThrowOutOfMemory(ctx);
    }

    qsort(ents, n, sizeof(*ents), dyn_dirent_cmp);

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        for (i = 0; i < n; i++)
            free(ents[i].name);
        free(ents);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        JSValue o = JS_NewObject(ctx);
        if (!JS_IsException(o)) {
            JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, ents[i].name));
            JS_SetPropertyStr(ctx, o, "isDir", JS_NewBool(ctx, ents[i].is_dir));
            JS_SetPropertyStr(ctx, o, "isFile",
                              JS_NewBool(ctx, ents[i].is_file));
            JS_SetPropertyStr(ctx, o, "isSymlink",
                              JS_NewBool(ctx, ents[i].is_symlink));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        free(ents[i].name);
    }
    free(ents);
    return arr;
}

/* ==================================================================== *
 *  makeDir / remove / removeAll / rename                                *
 * ==================================================================== */

/* mkdir -p: create `path` and any missing parents. Returns 0 or -1 (errno set,
 * and matches the failing mkdir's errno). An existing directory is success. */
static int dyn_fs_mkdirp(const char *path, mode_t mode)
{
    struct stat st;
    size_t len;
    char *parent;
    int r;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST; /* exists but is not a directory */
        return -1;
    }
    if (errno != ENOENT)
        return -1;

    /* create the parent, then retry */
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/')
        len--;
    while (len > 0 && path[len - 1] != '/')
        len--;
    while (len > 0 && path[len - 1] == '/')
        len--;
    if (len == 0) {
        errno = ENOENT;
        return -1;
    }
    parent = (char *)malloc(len + 1);
    if (!parent) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    r = dyn_fs_mkdirp(parent, mode);
    free(parent);
    if (r != 0)
        return -1;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
    }
    return -1;
}

static JSValue dyn_fs_make_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *path;
    int recursive = 0;
    int32_t mode = 0777;
    int r;

    (void)this_val;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "mode");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt32(ctx, &mode, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, path);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    if (recursive)
        r = dyn_fs_mkdirp(path, (mode_t)mode);
    else
        r = mkdir(path, (mode_t)mode);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "makeDir", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* remove(path): unlink a file or an EMPTY directory (libc remove()). */
static JSValue dyn_fs_remove(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *path;
    (void)this_val; (void)argc;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (remove(path) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "remove", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* Remove everything UNDER the directory referred to by `dirfd`, recursively,
 * without ever following a symlink. Takes ownership of `dirfd` (closes it).
 * Returns 0 or -1 (errno set). */
static int dyn_fs_rmrf_children(int dirfd, int depth)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        close(dirfd);
        errno = ELOOP;
        return -1;
    }
    /* fdopendir takes ownership of dirfd (closedir closes it). */
    d = fdopendir(dirfd);
    if (!d) {
        int e2 = errno;
        close(dirfd);
        errno = e2;
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int cfd = openat(dirfd, e->d_name,
                             O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            if (cfd < 0) {
                rc = -1;
                break;
            }
            if (dyn_fs_rmrf_children(cfd, depth + 1) != 0) {
                rc = -1;
                break;
            }
            if (unlinkat(dirfd, e->d_name, AT_REMOVEDIR) != 0) {
                rc = -1;
                break;
            }
        } else {
            if (unlinkat(dirfd, e->d_name, 0) != 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(d); /* closes dirfd */
    return rc;
}

/* removeAll(path): recursive, symlink-safe, missing path is a no-op. */
static JSValue dyn_fs_remove_all(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    struct stat st;

    (void)this_val; (void)argc;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    if (lstat(path, &st) != 0) {
        int e = errno;
        JS_FreeCString(ctx, path);
        if (e == ENOENT)
            return JS_UNDEFINED; /* nothing to remove */
        return dyn_fs_throw(ctx, e, "removeAll", NULL);
    }

    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
        if (dyn_fs_rmrf_children(fd, 0) != 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
        if (rmdir(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_fs_rename(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *from, *to;
    (void)this_val; (void)argc;

    from = JS_ToCString(ctx, argv[0]);
    if (!from)
        return JS_EXCEPTION;
    to = JS_ToCString(ctx, argv[1]);
    if (!to) {
        JS_FreeCString(ctx, from);
        return JS_EXCEPTION;
    }
    if (rename(from, to) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "rename", from);
        JS_FreeCString(ctx, from);
        JS_FreeCString(ctx, to);
        return ex;
    }
    JS_FreeCString(ctx, from);
    JS_FreeCString(ctx, to);
    return JS_UNDEFINED;
}

/* ==================================================================== *
 *  symlink / readLink / realPath / chmod                                *
 * ==================================================================== */

static JSValue dyn_fs_symlink(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *target, *linkpath;
    (void)this_val; (void)argc;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    linkpath = JS_ToCString(ctx, argv[1]);
    if (!linkpath) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    if (symlink(target, linkpath) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "symlink", linkpath);
        JS_FreeCString(ctx, target);
        JS_FreeCString(ctx, linkpath);
        return ex;
    }
    JS_FreeCString(ctx, target);
    JS_FreeCString(ctx, linkpath);
    return JS_UNDEFINED;
}

static JSValue dyn_fs_read_link(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *path;
    size_t cap = 256;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    for (;;) {
        char *buf = (char *)malloc(cap);
        ssize_t n;
        if (!buf) {
            JS_FreeCString(ctx, path);
            return JS_ThrowOutOfMemory(ctx);
        }
        n = readlink(path, buf, cap);
        if (n < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "readLink", path);
            free(buf);
            JS_FreeCString(ctx, path);
            return ex;
        }
        if ((size_t)n < cap) {
            JSValue out = JS_NewStringLen(ctx, buf, (size_t)n);
            free(buf);
            JS_FreeCString(ctx, path);
            return out;
        }
        /* result filled the buffer: it may be truncated -- grow and retry */
        free(buf);
        cap *= 2;
        if (cap > DYN_FS_READLINK_MAX) {
            JSValue ex = dyn_fs_throw(ctx, ENAMETOOLONG, "readLink", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    }
}

static JSValue dyn_fs_real_path(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *path;
    char *resolved;
    JSValue out;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    resolved = realpath(path, NULL); /* POSIX.1-2008: NULL => malloc'd result */
    if (!resolved) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "realPath", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    out = JS_NewString(ctx, resolved);
    free(resolved);
    return out;
}

static JSValue dyn_fs_chmod(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    const char *path;
    int32_t mode;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &mode, argv[1])) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    if (chmod(path, (mode_t)mode) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "chmod", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ==================================================================== *
 *  glob -- self-contained *, **, ?, [...] matcher + directory walk      *
 * ==================================================================== */

/* Match a single path component `s` against pattern `p` (which contains no
 * '/'). Supports *, ?, and [...] classes (with ranges, and ! or ^ negation).
 * An unterminated '[' is treated as a literal '['. */
static int dyn_glob_match(const char *p, const char *s)
{
    for (;;) {
        unsigned char pc = (unsigned char)*p;
        if (pc == '\0')
            return *s == '\0';
        if (pc == '*') {
            const char *t;
            while (*p == '*')
                p++;
            if (*p == '\0')
                return 1; /* trailing '*' matches the rest */
            for (t = s;; t++) {
                if (dyn_glob_match(p, t))
                    return 1;
                if (*t == '\0')
                    return 0;
            }
        }
        if (*s == '\0')
            return 0; /* remaining pattern needs at least one char */
        if (pc == '?') {
            p++; s++;
            continue;
        }
        if (pc == '[') {
            const char *q = p + 1;
            const char *start;
            int negate = 0, matched = 0;
            unsigned char c = (unsigned char)*s;
            if (*q == '!' || *q == '^') {
                negate = 1;
                q++;
            }
            start = q;
            while (*q && !(*q == ']' && q != start)) {
                if (q[0] && q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char lo = (unsigned char)q[0];
                    unsigned char hi = (unsigned char)q[2];
                    if (lo <= c && c <= hi)
                        matched = 1;
                    q += 3;
                } else {
                    if ((unsigned char)*q == c)
                        matched = 1;
                    q++;
                }
            }
            if (*q != ']') {
                /* unterminated class: treat '[' as a literal character */
                if (c != '[')
                    return 0;
                p++; s++;
                continue;
            }
            q++; /* consume ']' */
            if (matched == negate)
                return 0;
            p = q; s++;
            continue;
        }
        /* literal byte */
        if (pc != (unsigned char)*s)
            return 0;
        p++; s++;
    }
}

/* Apply the minimatch "leading dot" rule: a wildcard metacharacter does not
 * match a name that begins with '.'; the segment must start with a literal
 * '.' to match a dotfile. */
static int dyn_glob_match_name(const char *pat, const char *name)
{
    if (name[0] == '.' && pat[0] != '.')
        return 0;
    return dyn_glob_match(pat, name);
}

static int dyn_glob_has_wildcard(const char *s)
{
    return strpbrk(s, "*?[") != NULL;
}

typedef struct {
    char **items;
    size_t count, cap;
    int oom;
} dyn_glob_res;

static void dyn_glob_res_push(dyn_glob_res *r, const char *s, size_t len)
{
    char *dup;
    if (r->oom)
        return;
    if (r->count == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 32;
        char **ni = (char **)realloc(r->items, nc * sizeof(*ni));
        if (!ni) {
            r->oom = 1;
            return;
        }
        r->items = ni;
        r->cap = nc;
    }
    dup = (char *)malloc(len + 1);
    if (!dup) {
        r->oom = 1;
        return;
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    r->items[r->count++] = dup;
}

static void dyn_glob_res_free(dyn_glob_res *r)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        free(r->items[i]);
    free(r->items);
}

/* Emit the fully-matched relative path `rel` in display form (prefixed with '/'
 * for an absolute pattern; "." for the empty relative root). */
static void dyn_glob_emit(dyn_glob_res *res, const char *rel, int is_abs)
{
    if (is_abs) {
        char *disp = dyn_join("/", rel); /* "/"+rel, or "/" when rel=="" */
        if (!disp) {
            res->oom = 1;
            return;
        }
        dyn_glob_res_push(res, disp, strlen(disp));
        free(disp);
    } else if (rel[0] == '\0') {
        dyn_glob_res_push(res, ".", 1);
    } else {
        dyn_glob_res_push(res, rel, strlen(rel));
    }
}

/* Compute the on-disk directory to list for the walk position `rel`. */
static char *dyn_glob_fsdir(const char *base, const char *rel)
{
    if (rel[0] == '\0')
        return dyn_join(base, ""); /* == a copy of base */
    return dyn_join(base, rel);
}

static void dyn_glob_walk(dyn_glob_res *res, const char *base, char **segs,
                          int nseg, int si, const char *rel, int is_abs,
                          int depth);

static void dyn_glob_walk(dyn_glob_res *res, const char *base, char **segs,
                          int nseg, int si, const char *rel, int is_abs,
                          int depth)
{
    const char *seg;
    char *fsdir;

    if (res->oom || depth > DYN_FS_GLOB_MAX_DEPTH)
        return;
    if (si == nseg) {
        dyn_glob_emit(res, rel, is_abs);
        return;
    }
    seg = segs[si];

    if (strcmp(seg, "**") == 0) {
        int is_last = (si == nseg - 1);
        DIR *d;
        struct dirent *e;

        /* '**' matching zero directories: advance to the next segment here.
         * As the last segment, '**' also matches the current directory itself
         * (when non-empty) plus its whole subtree (files and dirs below). */
        if (is_last) {
            if (rel[0] != '\0')
                dyn_glob_emit(res, rel, is_abs);
        } else {
            dyn_glob_walk(res, base, segs, nseg, si + 1, rel, is_abs, depth);
        }

        fsdir = dyn_glob_fsdir(base, rel);
        if (!fsdir) {
            res->oom = 1;
            return;
        }
        d = opendir(fsdir);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char *childrel, *childfs;
                struct stat st;
                if (dyn_is_dot_or_dotdot(e->d_name) || e->d_name[0] == '.')
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) { res->oom = 1; break; }
                childfs = dyn_join(fsdir, e->d_name);
                if (!childfs) { free(childrel); res->oom = 1; break; }
                /* '**' spans into real subdirectories only (never a symlink,
                 * so a symlink cycle cannot recurse forever). */
                if (lstat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                    dyn_glob_walk(res, base, segs, nseg, si, childrel, is_abs,
                                  depth + 1);
                else if (is_last)
                    dyn_glob_emit(res, childrel, is_abs); /* trailing-'**' file */
                free(childfs);
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
        free(fsdir);
        return;
    }

    /* literal segment (no wildcard): probe the specific child directly, so a
     * component we cannot list (permission) still resolves an explicit name. */
    if (!dyn_glob_has_wildcard(seg)) {
        char *childrel = (rel[0] == '\0') ? dyn_join("", seg)
                                          : dyn_join(rel, seg);
        char *childfs;
        struct stat st;
        if (!childrel) { res->oom = 1; return; }
        childfs = dyn_glob_fsdir(base, childrel);
        if (!childfs) { free(childrel); res->oom = 1; return; }
        if (si == nseg - 1) {
            if (lstat(childfs, &st) == 0)
                dyn_glob_emit(res, childrel, is_abs);
        } else {
            if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                dyn_glob_walk(res, base, segs, nseg, si + 1, childrel, is_abs,
                              depth + 1);
        }
        free(childfs);
        free(childrel);
        return;
    }

    /* wildcard segment: list the directory and match each entry. */
    fsdir = dyn_glob_fsdir(base, rel);
    if (!fsdir) {
        res->oom = 1;
        return;
    }
    {
        DIR *d = opendir(fsdir);
        int is_last = (si == nseg - 1);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char *childrel;
                if (dyn_is_dot_or_dotdot(e->d_name))
                    continue;
                if (!dyn_glob_match_name(seg, e->d_name))
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) { res->oom = 1; break; }
                if (is_last) {
                    dyn_glob_emit(res, childrel, is_abs);
                } else {
                    char *childfs = dyn_join(fsdir, e->d_name);
                    struct stat st;
                    if (!childfs) { free(childrel); res->oom = 1; break; }
                    if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                        dyn_glob_walk(res, base, segs, nseg, si + 1, childrel,
                                      is_abs, depth + 1);
                    free(childfs);
                }
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
    }
    free(fsdir);
}

static int dyn_glob_str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static JSValue dyn_fs_glob(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    const char *pattern = NULL, *cwd = NULL;
    char *patcopy = NULL, **segs = NULL;
    int nseg = 0, is_abs, i;
    size_t plen;
    const char *base;
    dyn_glob_res res;
    JSValue arr;
    char *p;

    (void)this_val;
    pattern = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pattern)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "cwd");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            cwd = JS_ToCString(ctx, v);
            if (!cwd) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, pattern);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    memset(&res, 0, sizeof(res));

    if (plen == 0) {
        /* empty pattern matches nothing */
        JS_FreeCString(ctx, pattern);
        if (cwd)
            JS_FreeCString(ctx, cwd);
        return JS_NewArray(ctx);
    }

    patcopy = (char *)malloc(plen + 1);
    segs = (char **)malloc((plen + 2) * sizeof(*segs));
    if (!patcopy || !segs) {
        free(patcopy);
        free(segs);
        JS_FreeCString(ctx, pattern);
        if (cwd)
            JS_FreeCString(ctx, cwd);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(patcopy, pattern, plen + 1);
    is_abs = (patcopy[0] == '/');

    /* split on '/', dropping empty segments (leading/trailing/duplicate '/') */
    p = patcopy;
    while (*p) {
        char *seg_start;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        seg_start = p;
        while (*p && *p != '/')
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        segs[nseg++] = seg_start;
    }

    base = is_abs ? "/" : ((cwd && cwd[0]) ? cwd : ".");
    dyn_glob_walk(&res, base, segs, nseg, 0, "", is_abs, 0);

    free(patcopy);
    free(segs);
    JS_FreeCString(ctx, pattern);
    if (cwd)
        JS_FreeCString(ctx, cwd);

    if (res.oom) {
        dyn_glob_res_free(&res);
        return JS_ThrowOutOfMemory(ctx);
    }

    /* sort, then drop adjacent duplicates (a pathological pattern with two
     * '**' segments can reach the same path twice). */
    qsort(res.items, res.count, sizeof(res.items[0]), dyn_glob_str_cmp);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_glob_res_free(&res);
        return JS_EXCEPTION;
    }
    {
        uint32_t out = 0;
        for (i = 0; i < (int)res.count; i++) {
            if (i > 0 && strcmp(res.items[i], res.items[i - 1]) == 0)
                continue;
            JS_SetPropertyUint32(ctx, arr, out++,
                                 JS_NewString(ctx, res.items[i]));
        }
    }
    dyn_glob_res_free(&res);
    return arr;
}

/* ==================================================================== *
 *  temp                                                                 *
 * ==================================================================== */

/* System temp directory (TMPDIR, else /tmp). Returned pointer is into environ
 * or a static literal -- valid transiently, do not free. */
static const char *dyn_fs_tempdir_str(void)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return t;
}

static JSValue dyn_fs_temp_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *t = dyn_fs_tempdir_str();
    size_t len = strlen(t);
    (void)this_val; (void)argc; (void)argv;
    /* strip a trailing '/' (but keep a bare "/") */
    while (len > 1 && t[len - 1] == '/')
        len--;
    return JS_NewStringLen(ctx, t, len);
}

/* Shared mkdtemp/mkstemp template builder: "<tmp>/<prefix>XXXXXX". */
static char *dyn_fs_temp_template(const char *prefix)
{
    const char *t = dyn_fs_tempdir_str();
    size_t tl = strlen(t), pl = prefix ? strlen(prefix) : 0;
    char *tpl;
    while (tl > 1 && t[tl - 1] == '/')
        tl--;
    tpl = (char *)malloc(tl + 1 + pl + 6 + 1);
    if (!tpl)
        return NULL;
    memcpy(tpl, t, tl);
    tpl[tl] = '/';
    if (pl)
        memcpy(tpl + tl + 1, prefix, pl);
    memcpy(tpl + tl + 1 + pl, "XXXXXX", 6);
    tpl[tl + 1 + pl + 6] = '\0';
    return tpl;
}

static JSValue dyn_fs_make_temp_dir(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *prefix = NULL;
    char *tpl;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    tpl = dyn_fs_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    if (!mkdtemp(tpl)) {
        int e = errno;
        free(tpl);
        return dyn_fs_throw(ctx, e, "makeTempDir", NULL);
    }
    out = JS_NewString(ctx, tpl);
    free(tpl);
    return out;
}

static JSValue dyn_fs_make_temp_file(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *prefix = NULL;
    char *tpl;
    int fd;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    tpl = dyn_fs_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    fd = mkstemp(tpl);
    if (fd < 0) {
        int e = errno;
        free(tpl);
        return dyn_fs_throw(ctx, e, "makeTempFile", NULL);
    }
    close(fd); /* leave an empty file, return its path */
    out = JS_NewString(ctx, tpl);
    free(tpl);
    return out;
}

static const JSCFunctionListEntry dyn_file_funcs[] = {
    /* content I/O */
    JS_CFUNC_DEF("readFile", 1, dyn_file_read_file),
    JS_CFUNC_DEF("writeFile", 2, dyn_file_write_file),
    /* metadata */
    JS_CFUNC_DEF("stat", 1, dyn_fs_stat),
    JS_CFUNC_DEF("lstat", 1, dyn_fs_lstat),
    JS_CFUNC_DEF("exists", 1, dyn_fs_exists),
    /* directories */
    JS_CFUNC_DEF("readDir", 1, dyn_fs_read_dir),
    JS_CFUNC_DEF("makeDir", 2, dyn_fs_make_dir),
    JS_CFUNC_DEF("remove", 1, dyn_fs_remove),
    JS_CFUNC_DEF("removeAll", 1, dyn_fs_remove_all),
    JS_CFUNC_DEF("rename", 2, dyn_fs_rename),
    /* links / perms */
    JS_CFUNC_DEF("symlink", 2, dyn_fs_symlink),
    JS_CFUNC_DEF("readLink", 1, dyn_fs_read_link),
    JS_CFUNC_DEF("realPath", 1, dyn_fs_real_path),
    JS_CFUNC_DEF("chmod", 2, dyn_fs_chmod),
    /* globbing */
    JS_CFUNC_DEF("glob", 2, dyn_fs_glob),
    /* temp */
    JS_CFUNC_DEF("tempDir", 0, dyn_fs_temp_dir),
    JS_CFUNC_DEF("makeTempDir", 1, dyn_fs_make_temp_dir),
    JS_CFUNC_DEF("makeTempFile", 1, dyn_fs_make_temp_file),
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
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:file", dyn_file_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "FileReader");
    JS_AddModuleExport(ctx, m, "FileWriter");
    JS_AddModuleExportList(ctx, m, dyn_file_funcs, (int)countof(dyn_file_funcs));
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_FILE */
