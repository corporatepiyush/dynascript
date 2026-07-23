/*
 * dynajs:sys -- unified system interface: filesystem metadata + directories +
 * globbing + process/environment. Self-contained, in-repo (no external deps),
 * synchronous, portable across macOS and Linux.
 *
 *   import * as sys from "dynajs:sys";
 *
 * This is deliberately NOT a node:fs / Go os clone. Where Node spreads this
 * surface across fs/os/path/process plus npm (glob, rimraf, mkdirp) and Go
 * across os/path/filepath/io/fs, dynajs:sys unifies the filesystem+process+
 * environment surface into ONE module with clean names. Path-STRING logic
 * (join/normalize/dirname/...) stays in dynajs:path; buffered file CONTENT I/O
 * (FileReader/FileWriter/readFile/writeFile) stays in dynajs:file -- this
 * module does metadata, directory structure, links, globbing and process/env.
 *
 * Surface:
 *   Metadata : stat(path), lstat(path) -> { size, mode, isDir, isFile,
 *              isSymlink, mtimeMs, atimeMs, ctimeMs, uid, gid, ino, nlink };
 *              exists(path) -> bool (never throws).
 *   Dirs     : readDir(path) -> [{ name, isDir, isFile, isSymlink }] sorted by
 *              name (skips "."/".."); makeDir(path, {recursive, mode});
 *              remove(path); removeAll(path); rename(from, to).
 *   Links    : symlink(target, linkPath), readLink(path), realPath(path),
 *              chmod(path, mode).
 *   Glob     : glob(pattern, {cwd}) -> sorted, de-duplicated array of matching
 *              paths (relative to cwd for relative patterns, absolute for
 *              absolute patterns). Supports * (never crosses '/'), ** (spans
 *              directories), ? and [...] classes -- implemented here, no libc
 *              fnmatch/glob dependency.
 *   Temp     : tempDir(), makeTempDir(prefix), makeTempFile(prefix).
 *   Process  : env(), getEnv(name), setEnv(name, val), args(), cwd(),
 *              chDir(path), platform(), pid(), hostName(), homeDir().
 *
 * Semantics / documented guarantees:
 *   - mode is the FULL st_mode (type bits + Unix permission bits, e.g. a 0755
 *     regular file reports 0100755). Mask with 0o777 for the permission bits.
 *   - Times are milliseconds since the epoch as floating-point (sub-ms from the
 *     nanosecond timespec is preserved).
 *   - stat() follows symlinks; lstat() does not (so isSymlink is only ever true
 *     from lstat / readDir entries).
 *   - exists(path) uses lstat: it reports whether a directory ENTRY exists at
 *     path and returns true for a dangling symlink (the link node exists). It
 *     never throws -- any error (incl. permission) yields false.
 *   - removeAll(path) is recursive and symlink-safe: it NEVER descends through a
 *     symlink out of the tree (children are removed via openat/unlinkat with
 *     O_NOFOLLOW + fstatat(AT_SYMLINK_NOFOLLOW), a symlink is unlinked not
 *     followed), a missing path is a no-op, and recursion depth is bounded.
 *   - glob never follows a symlinked directory while expanding a '**', so a
 *     symlink cycle cannot cause infinite recursion; a leading '.' in a name is
 *     only matched by an explicit leading '.' in the segment (minimatch rule).
 *
 * Coercion discipline (CLAUDE.md sec.5): every method coerces ALL of its JS
 * arguments into owned C locals (JS_ToCString / JS_ToInt32 / JS_ToBool) FIRST,
 * then performs the syscall. These are transient plain functions -- no `this`,
 * no long-lived native handle -- so there is no resource for a reentrant
 * valueOf/toString to corrupt; the discipline here is simply that every
 * JS_ToCString result is released on every path, including every error path.
 */
#include "dynajs-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define dyn_environ (*_NSGetEnviron())
#else
extern char **environ;
#define dyn_environ environ
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Bounds on recursive filesystem walks (removeAll / glob). Real directory
 * trees are far shallower; these only guard against a pathological/adversarial
 * nesting so the JS-thread C stack can never overflow. */
#define DYN_SYS_RMRF_MAX_DEPTH 512
#define DYN_SYS_GLOB_MAX_DEPTH 512
#define DYN_SYS_READLINK_MAX   (1u << 16)

/* ==================================================================== *
 *  errno -> thrown JS Error (descriptive message + .code + .errno)      *
 * ==================================================================== */

static const char *dyn_sys_errno_code(int e)
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
static JSValue dyn_sys_throw(JSContext *ctx, int e, const char *op,
                             const char *path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char *code = dyn_sys_errno_code(e);

    if (path)
        snprintf(msg, sizeof(msg), "sys.%s(\"%s\"): %s", op, path, strerror(e));
    else
        snprintf(msg, sizeof(msg), "sys.%s: %s", op, strerror(e));

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

static JSValue dyn_sys_stat_common(JSContext *ctx, JSValueConst arg, int follow)
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
        JSValue ex = dyn_sys_throw(ctx, e, follow ? "stat" : "lstat", path);
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

static JSValue dyn_sys_stat(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_sys_stat_common(ctx, argv[0], 1);
}

static JSValue dyn_sys_lstat(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_sys_stat_common(ctx, argv[0], 0);
}

/* exists(path) -> bool. Never throws: any error (missing, permission, ...)
 * yields false. Uses lstat, so a dangling symlink reports true. */
static JSValue dyn_sys_exists(JSContext *ctx, JSValueConst this_val, int argc,
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

static JSValue dyn_sys_read_dir(JSContext *ctx, JSValueConst this_val, int argc,
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
        JSValue ex = dyn_sys_throw(ctx, en, "readDir", path);
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
static int dyn_sys_mkdirp(const char *path, mode_t mode)
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
    r = dyn_sys_mkdirp(parent, mode);
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

static JSValue dyn_sys_make_dir(JSContext *ctx, JSValueConst this_val, int argc,
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
        r = dyn_sys_mkdirp(path, (mode_t)mode);
    else
        r = mkdir(path, (mode_t)mode);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "makeDir", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* remove(path): unlink a file or an EMPTY directory (libc remove()). */
static JSValue dyn_sys_remove(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *path;
    (void)this_val; (void)argc;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (remove(path) != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "remove", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* Remove everything UNDER the directory referred to by `dirfd`, recursively,
 * without ever following a symlink. Takes ownership of `dirfd` (closes it).
 * Returns 0 or -1 (errno set). */
static int dyn_sys_rmrf_children(int dirfd, int depth)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (depth > DYN_SYS_RMRF_MAX_DEPTH) {
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
            if (dyn_sys_rmrf_children(cfd, depth + 1) != 0) {
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
static JSValue dyn_sys_remove_all(JSContext *ctx, JSValueConst this_val,
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
        return dyn_sys_throw(ctx, e, "removeAll", NULL);
    }

    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            int e = errno;
            JSValue ex = dyn_sys_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
        if (dyn_sys_rmrf_children(fd, 0) != 0) {
            int e = errno;
            JSValue ex = dyn_sys_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
        if (rmdir(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_sys_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_sys_throw(ctx, e, "removeAll", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_rename(JSContext *ctx, JSValueConst this_val, int argc,
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
        JSValue ex = dyn_sys_throw(ctx, e, "rename", from);
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

static JSValue dyn_sys_symlink(JSContext *ctx, JSValueConst this_val, int argc,
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
        JSValue ex = dyn_sys_throw(ctx, e, "symlink", linkpath);
        JS_FreeCString(ctx, target);
        JS_FreeCString(ctx, linkpath);
        return ex;
    }
    JS_FreeCString(ctx, target);
    JS_FreeCString(ctx, linkpath);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_read_link(JSContext *ctx, JSValueConst this_val,
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
            JSValue ex = dyn_sys_throw(ctx, e, "readLink", path);
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
        if (cap > DYN_SYS_READLINK_MAX) {
            JSValue ex = dyn_sys_throw(ctx, ENAMETOOLONG, "readLink", path);
            JS_FreeCString(ctx, path);
            return ex;
        }
    }
}

static JSValue dyn_sys_real_path(JSContext *ctx, JSValueConst this_val,
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
        JSValue ex = dyn_sys_throw(ctx, e, "realPath", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    out = JS_NewString(ctx, resolved);
    free(resolved);
    return out;
}

static JSValue dyn_sys_chmod(JSContext *ctx, JSValueConst this_val, int argc,
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
        JSValue ex = dyn_sys_throw(ctx, e, "chmod", path);
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

    if (res->oom || depth > DYN_SYS_GLOB_MAX_DEPTH)
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

static JSValue dyn_sys_glob(JSContext *ctx, JSValueConst this_val, int argc,
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
static const char *dyn_sys_tempdir_str(void)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return t;
}

static JSValue dyn_sys_temp_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *t = dyn_sys_tempdir_str();
    size_t len = strlen(t);
    (void)this_val; (void)argc; (void)argv;
    /* strip a trailing '/' (but keep a bare "/") */
    while (len > 1 && t[len - 1] == '/')
        len--;
    return JS_NewStringLen(ctx, t, len);
}

/* Shared mkdtemp/mkstemp template builder: "<tmp>/<prefix>XXXXXX". */
static char *dyn_sys_temp_template(const char *prefix)
{
    const char *t = dyn_sys_tempdir_str();
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

static JSValue dyn_sys_make_temp_dir(JSContext *ctx, JSValueConst this_val,
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
    tpl = dyn_sys_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    if (!mkdtemp(tpl)) {
        int e = errno;
        free(tpl);
        return dyn_sys_throw(ctx, e, "makeTempDir", NULL);
    }
    out = JS_NewString(ctx, tpl);
    free(tpl);
    return out;
}

static JSValue dyn_sys_make_temp_file(JSContext *ctx, JSValueConst this_val,
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
    tpl = dyn_sys_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    fd = mkstemp(tpl);
    if (fd < 0) {
        int e = errno;
        free(tpl);
        return dyn_sys_throw(ctx, e, "makeTempFile", NULL);
    }
    close(fd); /* leave an empty file, return its path */
    out = JS_NewString(ctx, tpl);
    free(tpl);
    return out;
}

/* ==================================================================== *
 *  process / environment                                                *
 * ==================================================================== */

static JSValue dyn_sys_env(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    char **envp = dyn_environ;
    JSValue obj;
    uint32_t idx;
    (void)this_val; (void)argc; (void)argv;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (idx = 0; envp[idx] != NULL; idx++) {
        const char *entry = envp[idx];
        const char *eq = strchr(entry, '=');
        JSAtom atom;
        if (!eq)
            continue;
        atom = JS_NewAtomLen(ctx, entry, (size_t)(eq - entry));
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValue(ctx, obj, atom, JS_NewString(ctx, eq + 1),
                               JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    return obj;
}

static JSValue dyn_sys_get_env(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *name, *val;
    JSValue out;
    (void)this_val; (void)argc;

    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = getenv(name);
    out = val ? JS_NewString(ctx, val) : JS_UNDEFINED;
    JS_FreeCString(ctx, name);
    return out;
}

static JSValue dyn_sys_set_env(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *name, *val;
    int r;
    (void)this_val; (void)argc;

    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = JS_ToCString(ctx, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = setenv(name, val, 1);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "setEnv", name);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        return ex;
    }
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

/* args() -> the process argument vector. macOS reads it via crt_externs;
 * Linux reads /proc/self/cmdline; elsewhere returns an empty array. */
static JSValue dyn_sys_args(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    JSValue arr;
    (void)this_val; (void)argc; (void)argv;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;

#if defined(__APPLE__)
    {
        int ac = *_NSGetArgc();
        char **av = *_NSGetArgv();
        int i;
        for (i = 0; i < ac && av && av[i]; i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewString(ctx, av[i]));
    }
#elif defined(__linux__)
    {
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char *buf = NULL;
            size_t len = 0, cap = 0;
            for (;;) {
                ssize_t got;
                if (len + 4096 > cap) {
                    size_t nc = cap ? cap * 2 : 8192;
                    char *nb = (char *)realloc(buf, nc);
                    if (!nb) { free(buf); buf = NULL; len = 0; break; }
                    buf = nb;
                    cap = nc;
                }
                got = read(fd, buf + len, cap - len);
                if (got < 0) {
                    if (errno == EINTR)
                        continue;
                    free(buf);
                    buf = NULL;
                    len = 0;
                    break;
                }
                if (got == 0)
                    break;
                len += (size_t)got;
            }
            close(fd);
            if (buf) {
                size_t i = 0;
                uint32_t idx = 0;
                while (i < len) {
                    size_t start = i;
                    while (i < len && buf[i] != '\0')
                        i++;
                    JS_SetPropertyUint32(ctx, arr, idx++,
                                         JS_NewStringLen(ctx, buf + start,
                                                         i - start));
                    i++; /* skip the NUL separator */
                }
                free(buf);
            }
        }
    }
#endif
    return arr;
}

static JSValue dyn_sys_cwd(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    char stackbuf[PATH_MAX];
    char *cwd;
    JSValue out;
    (void)this_val; (void)argc; (void)argv;

    cwd = getcwd(stackbuf, sizeof(stackbuf));
    if (cwd)
        return JS_NewString(ctx, cwd);
    if (errno != ERANGE)
        return dyn_sys_throw(ctx, errno, "cwd", NULL);
    /* path longer than PATH_MAX: grow until it fits */
    {
        size_t cap = sizeof(stackbuf);
        for (;;) {
            char *buf;
            cap *= 2;
            if (cap > (1u << 20))
                return dyn_sys_throw(ctx, ENAMETOOLONG, "cwd", NULL);
            buf = (char *)malloc(cap);
            if (!buf)
                return JS_ThrowOutOfMemory(ctx);
            if (getcwd(buf, cap)) {
                out = JS_NewString(ctx, buf);
                free(buf);
                return out;
            }
            free(buf);
            if (errno != ERANGE)
                return dyn_sys_throw(ctx, errno, "cwd", NULL);
        }
    }
}

static JSValue dyn_sys_chdir(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    const char *path;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (chdir(path) != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "chDir", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_platform(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
#if defined(__APPLE__)
    return JS_NewString(ctx, "darwin");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue dyn_sys_pid(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int32_t)getpid());
}

static JSValue dyn_sys_host_name(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    char buf[256];
    (void)this_val; (void)argc; (void)argv;
    if (gethostname(buf, sizeof(buf)) != 0)
        return dyn_sys_throw(ctx, errno, "hostName", NULL);
    buf[sizeof(buf) - 1] = '\0'; /* gethostname may not NUL-terminate on trunc */
    return JS_NewString(ctx, buf);
}

static JSValue dyn_sys_home_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *h;
    struct passwd *pw;
    (void)this_val; (void)argc; (void)argv;

    h = getenv("HOME");
    if (h && *h)
        return JS_NewString(ctx, h);
    pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return JS_NewString(ctx, pw->pw_dir);
    return dyn_sys_throw(ctx, ENOENT, "homeDir", NULL);
}

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

static const JSCFunctionListEntry dyn_sys_funcs[] = {
    /* metadata */
    JS_CFUNC_DEF("stat", 1, dyn_sys_stat),
    JS_CFUNC_DEF("lstat", 1, dyn_sys_lstat),
    JS_CFUNC_DEF("exists", 1, dyn_sys_exists),
    /* directories */
    JS_CFUNC_DEF("readDir", 1, dyn_sys_read_dir),
    JS_CFUNC_DEF("makeDir", 2, dyn_sys_make_dir),
    JS_CFUNC_DEF("remove", 1, dyn_sys_remove),
    JS_CFUNC_DEF("removeAll", 1, dyn_sys_remove_all),
    JS_CFUNC_DEF("rename", 2, dyn_sys_rename),
    /* links / perms */
    JS_CFUNC_DEF("symlink", 2, dyn_sys_symlink),
    JS_CFUNC_DEF("readLink", 1, dyn_sys_read_link),
    JS_CFUNC_DEF("realPath", 1, dyn_sys_real_path),
    JS_CFUNC_DEF("chmod", 2, dyn_sys_chmod),
    /* globbing */
    JS_CFUNC_DEF("glob", 2, dyn_sys_glob),
    /* temp */
    JS_CFUNC_DEF("tempDir", 0, dyn_sys_temp_dir),
    JS_CFUNC_DEF("makeTempDir", 1, dyn_sys_make_temp_dir),
    JS_CFUNC_DEF("makeTempFile", 1, dyn_sys_make_temp_file),
    /* process / environment */
    JS_CFUNC_DEF("env", 0, dyn_sys_env),
    JS_CFUNC_DEF("getEnv", 1, dyn_sys_get_env),
    JS_CFUNC_DEF("setEnv", 2, dyn_sys_set_env),
    JS_CFUNC_DEF("args", 0, dyn_sys_args),
    JS_CFUNC_DEF("cwd", 0, dyn_sys_cwd),
    JS_CFUNC_DEF("chDir", 1, dyn_sys_chdir),
    JS_CFUNC_DEF("platform", 0, dyn_sys_platform),
    JS_CFUNC_DEF("pid", 0, dyn_sys_pid),
    JS_CFUNC_DEF("hostName", 0, dyn_sys_host_name),
    JS_CFUNC_DEF("homeDir", 0, dyn_sys_home_dir),
};

static int dyn_sys_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

int js_nat_init_sys(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dynajs:sys", dyn_sys_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SYS */
