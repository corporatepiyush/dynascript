/*
 * dynascript native modules -- shared framework (in-repo, no external deps).
 * See dynajs-nat.h for the resource/ownership model.
 */
#include "dynajs-nat.h"

#ifdef CONFIG_NATIVE_MODULES

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ==================================================================== *
 *  Shared optimized disk-I/O primitives (used by dynajs:file, dynajs:csv,
 *  and any module that reads/writes whole files). Each uses the best
 *  facility the platform offers -- io_uring/fadvise/fallocate/fdatasync on
 *  Linux; F_RDAHEAD/F_RDADVISE/F_PREALLOCATE/F_FULLFSYNC on macOS.
 * ==================================================================== */

/* Hint that fd will be read sequentially and warm read-ahead. */
void dyn_io_advise_seq_read(int fd, off_t size)
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
        fcntl(fd, F_RDADVISE, &ra); /* async prefetch */
    }
#else
    (void)fd; (void)size;
#endif
}

/* Best-effort reserve `size` bytes of backing store (less fragmentation, no
 * ENOSPC mid-write). Returns 0. */
int dyn_io_preallocate(int fd, off_t size)
{
    if (size <= 0) return 0;
#if defined(__linux__)
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
    (void)fd; return 0;
#endif
}

/* Durably flush written data to stable storage (F_FULLFSYNC on macOS is the
 * only real flush-to-platter; plain fsync there only reaches the drive cache). */
int dyn_io_durable_sync(int fd)
{
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd);
#elif defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

/* Read the whole file the fastest way this platform offers (io_uring on Linux
 * when built with it, an advise-hinted sequential read otherwise). Returns 0
 * (caller free()s *out) or -1. */
int dyn_io_read_whole(const char *path, char **out, size_t *outlen)
{
#if defined(CONFIG_IO_URING) && defined(__linux__)
    return dyn_uring_read_all(path, out, outlen);
#else
    struct stat st;
    char *buf;
    size_t off = 0, size;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    dyn_io_advise_seq_read(fd, st.st_size);
    size = (size_t)st.st_size;
    buf = (char *)malloc(size ? size : 1);
    if (!buf) { close(fd); return -1; }
    while (off < size) {
        ssize_t r = read(fd, buf + off, size - off);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    *out = buf; *outlen = off;
    return 0;
#endif
}

/* Registry of framework-owned class ids, so the shared close()/closed methods
 * can validate `this` before treating its opaque as a DynResource. A foreign
 * object passed via close.call(x) must never be reinterpreted (it may store a
 * non-pointer opaque). Registration happens only from js_nat_init_all() on the
 * main context at startup -- worker threads never register modules -- so this
 * table is not concurrently mutated. Class ids are process-unique and never
 * reset; the cap is sized well beyond the number of native classes. */
#define DYN_MAX_CLASSES 256
static JSClassID dyn_class_ids[DYN_MAX_CLASSES];
static int dyn_n_classes;

static int dyn_is_our_class(JSClassID id)
{
    int i;
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == id)
            return 1;
    return 0;
}

/* Idempotent teardown: run the module dispose exactly once. */
static void dyn_res_release(JSRuntime *rt, DynResource *r)
{
    (void)rt;
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native);
    r->native = NULL;
}

void dyn_res_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(val, &id);
    /* only reached for our classes (set in their JSClassDef), so `r` is ours */
    if (r) {
        dyn_res_release(rt, r);
        js_free_rt(rt, r);
    }
}

JSValue dyn_res_wrap(JSContext *ctx, JSClassID class_id, void *native,
                     DynDisposeFunc dispose)
{
    JSValue obj;
    DynResource *r;

    obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj))
        goto fail;
    r = js_mallocz(ctx, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    r->native = native;
    r->dispose = dispose;
    r->closed = 0;
    JS_SetOpaque(obj, r);
    return obj;
 fail:
    if (dispose && native)
        dispose(native);
    return JS_EXCEPTION;
}

DynResource *dyn_res_get(JSContext *ctx, JSValueConst this_val,
                         JSClassID class_id)
{
    DynResource *r = JS_GetOpaque2(ctx, this_val, class_id);
    if (!r)
        return NULL; /* JS_GetOpaque2 already threw */
    if (r->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return NULL;
    }
    return r;
}

/* close() / dispose(): explicit deterministic release. Safe for any `this`. */
static JSValue dyn_method_close(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    (void)argc; (void)argv;
    if (r && dyn_is_our_class(id))
        dyn_res_release(JS_GetRuntime(ctx), r);
    return JS_UNDEFINED;
}

static JSValue dyn_getter_closed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    if (r && dyn_is_our_class(id))
        return JS_NewBool(ctx, r->closed);
    return JS_ThrowTypeError(ctx, "not a native resource");
}

/* close/dispose/[Symbol.dispose]/closed installed on every resource proto.
 * [Symbol.dispose] makes these work with `using` and DisposableStack.use(). */
static const JSCFunctionListEntry dyn_common_funcs[] = {
    JS_CFUNC_DEF("close", 0, dyn_method_close),
    JS_CFUNC_DEF("dispose", 0, dyn_method_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, dyn_method_close),
    JS_CGETSET_DEF("closed", dyn_getter_closed, NULL),
};

void dyn_res_class_common(JSContext *ctx, JSClassID class_id, JSValue proto)
{
    if (dyn_n_classes < DYN_MAX_CLASSES)
        dyn_class_ids[dyn_n_classes++] = class_id;
    JS_SetPropertyFunctionList(ctx, proto, dyn_common_funcs,
                               countof(dyn_common_funcs));
}

int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
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
    dyn_res_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

int js_nat_init_all(JSContext *ctx)
{
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES
    if (js_nat_init_structures(ctx))
        return -1;
    if (js_nat_init_structures_ext(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_HTTP
    if (js_nat_init_http(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
    if (js_nat_init_ml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DOCPARSE
    if (js_nat_init_docparse(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
    if (js_nat_init_compress(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
    if (js_nat_init_random(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SORT
    if (js_nat_init_sort(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SEARCH
    if (js_nat_init_search(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
    if (js_nat_init_structures3(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CONTAINER
    if (js_nat_init_container(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
    if (js_nat_init_simd(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
    if (js_nat_init_file(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TEXT
    if (js_nat_init_text(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_PATH
    if (js_nat_init_path(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_NETIP
    if (js_nat_init_netip(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SEMVER
    if (js_nat_init_semver(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_STRINGS
    if (js_nat_init_strings(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_BYTES
    if (js_nat_init_bytes(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CRYPTO
    if (js_nat_init_crypto(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ENCODING
    if (js_nat_init_encoding(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TIME
    if (js_nat_init_time(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_MATHX
    if (js_nat_init_mathx(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CSV
    if (js_nat_init_csv(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_UUID
    if (js_nat_init_uuid(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_BITS
    if (js_nat_init_bits(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
    if (js_nat_init_sys(ctx))
        return -1;
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
    if (js_nat_init_uring(ctx))
        return -1;
#endif
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES */
