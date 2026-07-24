/*
 * dyna-aio -- the project-wide async IO adapter: ONE proxy interface over the
 * fixed OS APIs (Linux io_uring, macOS/BSD kqueue), so every disk/network call
 * in the engine goes through an optimized, zero-extra-copy path with a single
 * portable surface. Simplicity of the adapter must NOT cost a copy or a syscall
 * the raw OS API wouldn't.
 *
 * MODEL: completion-oriented (submit an op, get a completion with a result),
 * because that maps 1:1 onto io_uring and cleanly subsumes kqueue:
 *   - io_uring: op -> SQE; completion <- CQE. SINGLE_ISSUER|DEFER_TASKRUN so
 *     completion task-work runs in the JS thread at reap time (no kernel worker
 *     touches JS state); provided-buffer ring for recv; registered fixed buffers
 *     + files for read/write/send; send_zc + splice/sendfile for zero-copy TX.
 *   - kqueue: the backend hides readiness-then-syscall behind the same
 *     completion callback, doing the recv/send/accept directly into/from the
 *     caller's (pool) buffer -- no extra copy. Regular-file disk (which kqueue
 *     cannot async) is serviced by OUR bounded thread pool, which posts its
 *     completion back through EVFILT_USER so it lands on the loop thread.
 *
 * THREADING: one dyn_aio per JS thread; ALL submission and completion on that
 * thread (io_uring SINGLE_ISSUER). The only other threads are the disk pool
 * workers, which never touch the JSContext -- they read/write into a plain
 * buffer and wake the loop. dyn_aio_backend_fd() folds the whole thing into
 * js_std_loop via js_std_set_io_reactor (see dyna-libc.h).
 *
 * kTLS/offload: a socket may be promoted to a kTLS ULP (setsockopt TLS_TX/RX);
 * send/sendfile/splice on it stay zero-copy. The adapter treats such an fd like
 * any other -- kTLS composes without a separate code path.
 */
#ifndef DYNAJS_AIO_H
#define DYNAJS_AIO_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "dyna-io.h" /* dyn_iobuf_t */

typedef struct dyn_aio dyn_aio_t;

/* Completion result codes: `res >= 0` is a byte count / accepted fd / 0-ok;
 * `res < 0` is -errno. `buf`/`buf_len` are set only for recv completions (a
 * BORROWED view into a pool buffer valid until the callback returns; the adapter
 * recycles it after). `udata` is the per-op cookie passed at submit. */
typedef void (*dyn_aio_cb)(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned buf_len, void *udata);

/* ---- lifecycle -------------------------------------------------------- */

/* Create the per-thread reactor (io_uring on Linux, kqueue+pool elsewhere).
 * `disk_workers` sizes the macOS/BSD disk thread pool (0 => a sensible default;
 * ignored where the kernel does async disk, i.e. io_uring). NULL on failure. */
dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers);
void dyn_aio_free(dyn_aio_t *aio);

/* The single pollable fd (io_uring eventfd / kqueue fd) to fold into an outer
 * event loop. Register with js_std_set_io_reactor(ctx, fd, dyn_aio_drain, aio). */
int dyn_aio_backend_fd(const dyn_aio_t *aio);

/* Reap all ready completions and invoke their callbacks (non-blocking). This is
 * the drain the JS loop calls when backend_fd signals -- its signature matches
 * js_std_set_io_reactor's drain hook exactly. */
void dyn_aio_drain(void *aio);
/* Standalone step: wait up to timeout_ms and dispatch. Returns #dispatched. */
int dyn_aio_run(dyn_aio_t *aio, int timeout_ms);

/* Count of ops still in flight (for loop-liveness / drain-before-close). */
size_t dyn_aio_inflight(const dyn_aio_t *aio);

/* ---- buffer pool (registered fixed buffers on io_uring) --------------- */

/* Register a pool of `n` buffers of `sz` bytes for recv (provided-buffer ring)
 * and for fixed read/write/send. Tiered sizing is the caller's job (e.g. a
 * small-header pool + a large-body pool). Returns a pool id >= 0, or -1. */
int dyn_aio_pool_register(dyn_aio_t *aio, unsigned n, unsigned sz);

/* ---- network ---------------------------------------------------------- */

/* Bind+listen a non-blocking TCP socket (SO_REUSEADDR/REUSEPORT, TCP_NODELAY on
 * accepted conns). Returns the listen fd or -1. `host` NULL => all interfaces. */
int dyn_aio_listen(dyn_aio_t *aio, const char *host, uint16_t port, int backlog);

/* Multishot accept: `cb` fires once per accepted connection (res = new fd), for
 * the life of the listener, until dyn_aio_cancel(). One submit, no re-arm. */
int dyn_aio_accept(dyn_aio_t *aio, int listen_fd, dyn_aio_cb cb, void *udata);

/* Recv into a pool buffer (io_uring provided-buffer / kqueue read). The callback
 * gets a borrowed view (buf,buf_len); res==0 => peer closed. Optionally multishot
 * (re-arms itself) via `multishot`. */
int dyn_aio_recv(dyn_aio_t *aio, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata);

/* Send `buf`/`len`. `flags`: DYN_AIO_ZC requests zero-copy (io_uring send_zc +
 * registered buffer); the adapter falls back to a plain send where unsupported.
 * The buffer must stay valid until the completion. */
#define DYN_AIO_ZC 1
int dyn_aio_send(dyn_aio_t *aio, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata);

/* Zero-copy file -> socket (io_uring splice / sendfile; kqueue sendfile). Used
 * by the static-file server and the WebSocket endpoint; composes with kTLS. */
int dyn_aio_sendfile(dyn_aio_t *aio, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata);

int dyn_aio_close(dyn_aio_t *aio, int fd);

/* ---- disk (io_uring on Linux; thread pool on macOS/BSD) --------------- */

/* Async open/read/write/fsync/statx. read/write use registered fixed buffers
 * when `buf` is pool-backed. `off < 0` means "current offset"/append semantics
 * where applicable. */
int dyn_aio_openat(dyn_aio_t *aio, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata);
int dyn_aio_read(dyn_aio_t *aio, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata);
int dyn_aio_write(dyn_aio_t *aio, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata);
int dyn_aio_fsync(dyn_aio_t *aio, int fd, int datasync, dyn_aio_cb cb, void *ud);

/* ---- cancel ----------------------------------------------------------- */

/* Cancel an outstanding multishot/op identified by its (cb,udata) cookie. */
int dyn_aio_cancel(dyn_aio_t *aio, dyn_aio_cb cb, void *udata);

#endif /* CONFIG_NATIVE_MODULES */
#endif /* DYNAJS_AIO_H */
