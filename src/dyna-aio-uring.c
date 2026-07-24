/*
 * dyna-aio (io_uring backend) -- the Linux completion-model implementation of
 * the dyn_aio interface (see dyna-aio.h). Selected by CONFIG_IO_URING; the
 * portable readiness backend (dyna-aio.c) is used otherwise. Same interface, so
 * the HTTP server and any dyn_aio user are backend-agnostic.
 *
 * Design: one ring per JS thread, SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN so
 * completion task-work runs in-thread at reap time. A registered eventfd is the
 * pollable fd folded into js_std_loop (dyn_aio_backend_fd); when it signals,
 * dyn_aio_drain reaps every CQE and invokes callbacks on the JS thread. Recv
 * uses a provided-buffer ring (kernel picks the buffer -> zero pre-post copy);
 * accept is multishot; send is a plain submit. user_data encodes (fd<<3)|op so a
 * completion maps back to the per-fd {cb,udata} slot.
 */
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <liburing.h>

#define URING_ENTRIES 8192
/* Provided-buffer pool = NBUFS*BUFSZ. Measured (kernel 7.0, arm64): buffers
 * recycle synchronously inside the drain, so few are held at once; -ENOBUFS
 * re-arms rather than drops. 512*8K=4MB matches epoll-class throughput at a
 * fraction of the old 4096*16K=64MB pool. Override with -D for bulk workloads. */
#ifndef URING_NBUFS
#define URING_NBUFS   512
#endif
#ifndef URING_BUFSZ
#define URING_BUFSZ   8192
#endif
#define URING_BGID    1

enum { UOP_ACCEPT = 0, UOP_RECV = 1, UOP_SEND = 2, UOP_CLOSE = 3 };
#define UD(fd, op)   (((uint64_t)(unsigned)(fd) << 3) | (unsigned)(op))
#define UD_FD(ud)    ((int)((ud) >> 3))
#define UD_OP(ud)    ((int)((ud) & 7))

typedef struct {
    dyn_aio_cb r_cb;   /* accept or recv completion */
    void *r_udata;
    uint8_t r_op;      /* UOP_ACCEPT / UOP_RECV / 0 */
    uint8_t r_multishot;
} uaio_fd_t;

/* One outstanding send. Heap-allocated so a connection can have several in
 * flight (e.g. a static response's header then body); the pointer travels in
 * the SQE user_data with the top bit set as a discriminator (Linux user-space
 * pointers are < 2^47, so the top bit is free). */
#define SEND_BIT (1ULL << 63)
typedef struct {
    uint8_t *buf;      /* copied payload, freed on completion */
    size_t len, off;   /* total and bytes already sent (partial-send resubmit) */
    int fd;
    dyn_aio_cb cb;
    void *udata;
} uaio_send_t;

struct dyn_aio {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    unsigned char *buf_base;
    unsigned nbufs, bufsz;
    int evfd;          /* registered eventfd: the pollable fd for js_std_loop */
    uaio_fd_t *fds;
    int cap;
    size_t inflight;
};

static int uaio_fd_ensure(dyn_aio_t *a, int fd)
{
    int nc;
    uaio_fd_t *nf;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 256;
    while (nc <= fd) nc *= 2;
    nf = (uaio_fd_t *)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf) return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

static struct io_uring_sqe *uaio_sqe(dyn_aio_t *a)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&a->ring);
    if (!sqe) { io_uring_submit(&a->ring); sqe = io_uring_get_sqe(&a->ring); }
    return sqe;
}

static void uaio_recycle(dyn_aio_t *a, int bid)
{
    io_uring_buf_ring_add(a->br, a->buf_base + (size_t)bid * a->bufsz, a->bufsz,
                          bid, io_uring_buf_ring_mask(a->nbufs), 0);
    io_uring_buf_ring_advance(a->br, 1);
}

/* ---- lifecycle ---- */

dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t *a = (dyn_aio_t *)calloc(1, sizeof(*a));
    struct io_uring_params p;
    unsigned i;
    int ret = 0;
    (void)entries; (void)disk_workers;
    if (!a) return NULL;
    a->nbufs = URING_NBUFS;
    a->bufsz = URING_BUFSZ;

    /* NOTE: do NOT use IORING_SETUP_DEFER_TASKRUN here -- it defers completion
     * processing until the ring is actively entered, so the registered eventfd
     * never fires and the poll(2)-based js_std_loop integration would hang.
     * SINGLE_ISSUER|COOP_TASKRUN keeps the single-thread optimization while
     * letting the eventfd notify on each completion. */
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
        memset(&p, 0, sizeof(p)); p.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
            memset(&p, 0, sizeof(p));
            if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
                free(a); return NULL;
            }
        }
    }
    a->buf_base = (unsigned char *)malloc((size_t)a->nbufs * a->bufsz);
    a->br = a->buf_base ? io_uring_setup_buf_ring(&a->ring, a->nbufs, URING_BGID, 0, &ret)
                        : NULL;
    a->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!a->buf_base || !a->br || a->evfd < 0 ||
        io_uring_register_eventfd(&a->ring, a->evfd) < 0) {
        if (a->br) io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
        if (a->evfd >= 0) close(a->evfd);
        free(a->buf_base);
        io_uring_queue_exit(&a->ring);
        free(a);
        return NULL;
    }
    for (i = 0; i < a->nbufs; i++)
        io_uring_buf_ring_add(a->br, a->buf_base + (size_t)i * a->bufsz, a->bufsz,
                              (int)i, io_uring_buf_ring_mask(a->nbufs), (int)i);
    io_uring_buf_ring_advance(a->br, a->nbufs);
    return a;
}

void dyn_aio_free(dyn_aio_t *a)
{
    if (!a) return;
    if (a->br) io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
    if (a->evfd >= 0) close(a->evfd);
    free(a->buf_base);
    io_uring_queue_exit(&a->ring);
    free(a->fds);
    free(a);
}

int dyn_aio_backend_fd(const dyn_aio_t *a) { return a->evfd; }
size_t dyn_aio_inflight(const dyn_aio_t *a) { return a->inflight; }

/* Re-arm a multishot recv (its slot is still armed; nothing to do but keep the
 * interest -- io_uring keeps multishot alive unless IORING_CQE_F_MORE is clear). */
static void uaio_rearm_recv(dyn_aio_t *a, int fd)
{
    struct io_uring_sqe *sqe = uaio_sqe(a);
    if (!sqe) return;
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV));
}

static void uaio_dispatch(dyn_aio_t *a, struct io_uring_cqe *cqe)
{
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    int fd, op, res = cqe->res;
    uaio_fd_t *s;

    if (ud & SEND_BIT) { /* a send completion (heap context pointer) */
        uaio_send_t *sc = (uaio_send_t *)(uintptr_t)(ud & ~SEND_BIT);
        if (res > 0) sc->off += (size_t)res;
        if ((res > 0 || res == -EAGAIN) && sc->off < sc->len) {
            /* partial send: resubmit the remainder (socket buffer was full) */
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) {
                io_uring_prep_send(sqe, sc->fd, sc->buf + sc->off,
                                   sc->len - sc->off, MSG_NOSIGNAL);
                io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)sc | SEND_BIT);
                io_uring_submit(&a->ring);
                return; /* keep sc alive until fully sent */
            }
        }
        { dyn_aio_cb cb = sc->cb; void *u = sc->udata;
          int result = res < 0 ? res : (int)sc->len;
          free(sc->buf); free(sc);
          if (a->inflight) a->inflight--;
          if (cb) cb(a, result, NULL, 0, u); }
        return;
    }
    fd = UD_FD(ud); op = UD_OP(ud);
    if (fd < 0 || fd >= a->cap) return;
    s = &a->fds[fd];

    if (op == UOP_ACCEPT) {
        dyn_aio_cb cb = s->r_cb; void *u = s->r_udata;
        if (res >= 0) {
            int on = 1;
            setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            if (cb) cb(a, res, NULL, 0, u);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && res != -ECANCELED) {
            /* multishot accept ended: re-arm */
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) { io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
                       io_uring_sqe_set_data64(sqe, UD(fd, UOP_ACCEPT)); }
        }
        return;
    }
    if (op == UOP_RECV) {
        dyn_aio_cb cb = s->r_cb; void *u = s->r_udata;
        int bid = (cqe->flags & IORING_CQE_F_BUFFER)
                      ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;
        if (res == -ENOBUFS) { /* pool momentarily empty: re-arm, do NOT close --
                                * buffers recycle within this same drain */
            uaio_rearm_recv(a, fd);
            return;
        }
        if (res > 0 && bid >= 0) {
            if (cb) cb(a, res, a->buf_base + (size_t)bid * a->bufsz, (unsigned)res, u);
            uaio_recycle(a, bid);
        } else {
            if (bid >= 0) uaio_recycle(a, bid);
            if (a->inflight) a->inflight--;
            s->r_op = 0;
            if (cb) cb(a, res <= 0 ? (res == 0 ? 0 : res) : 0, NULL, 0, u); /* closed/err */
            return;
        }
        if (!(cqe->flags & IORING_CQE_F_MORE)) /* multishot recv ended: re-arm */
            uaio_rearm_recv(a, fd);
        return;
    }
    /* UOP_CLOSE and any other op: nothing */
    (void)op;
}

void dyn_aio_drain(void *aio)
{
    dyn_aio_t *a = (dyn_aio_t *)aio;
    struct io_uring_cqe *cqe;
    unsigned head, n = 0;
    uint64_t v;
    ssize_t rd = read(a->evfd, &v, sizeof(v)); /* clear the eventfd counter */
    (void)rd;
    io_uring_submit(&a->ring);
    io_uring_for_each_cqe(&a->ring, head, cqe) { uaio_dispatch(a, cqe); n++; }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring); /* flush any SQEs queued by the callbacks */
}

int dyn_aio_run(dyn_aio_t *a, int timeout_ms)
{
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts, *pts = NULL;
    unsigned head, n = 0;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    io_uring_submit_and_wait_timeout(&a->ring, &cqe, 1, pts, NULL);
    io_uring_for_each_cqe(&a->ring, head, cqe) { uaio_dispatch(a, cqe); n++; }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring);
    return (int)n;
}

/* ---- network ---- */

int dyn_aio_listen(dyn_aio_t *a, const char *host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 1024) < 0) { close(fd); return -1; }
    return fd;
}

int dyn_aio_accept(dyn_aio_t *a, int listen_fd, dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;
    if (uaio_fd_ensure(a, listen_fd) < 0) return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_ACCEPT; s->r_multishot = 1;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD(listen_fd, UOP_ACCEPT));
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_recv(dyn_aio_t *a, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;
    (void)pool; (void)multishot;
    if (uaio_fd_ensure(a, fd) < 0) return -1;
    s = &a->fds[fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_RECV; s->r_multishot = 1;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV));
    return 0;
}

int dyn_aio_send(dyn_aio_t *a, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_send_t *sc;
    (void)flags;
    /* the payload must outlive the async send: copy it into a heap context so a
     * connection can have several sends (header, body, ...) in flight at once */
    sc = (uaio_send_t *)malloc(sizeof(*sc));
    if (!sc) return -1;
    sc->buf = (uint8_t *)malloc(len ? len : 1);
    if (!sc->buf) { free(sc); return -1; }
    memcpy(sc->buf, buf, len);
    sc->len = len; sc->off = 0; sc->fd = fd;
    sc->cb = cb; sc->udata = udata;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) { free(sc->buf); free(sc); a->inflight--; return -1; }
    io_uring_prep_send(sqe, fd, sc->buf, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)sc | SEND_BIT);
    io_uring_submit(&a->ring); /* submit now: sends are queued from timer/promise
                                * callbacks too, not only from a drain */
    return 0;
}

int dyn_aio_sendfile(dyn_aio_t *a, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata)
{
    /* Read the file (bounded by the static maxFileSize) and queue it as an
     * ordered io_uring send after the header. True zero-copy io_uring splice is
     * a follow-up; this preserves the completion model + ordering correctly. */
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    off_t off = offset;
    size_t got = 0;
    int rc = 0;
    if (!buf) { close(in_fd); if (cb) cb(a, -1, NULL, 0, udata); return 0; }
    while (got < len) {
        ssize_t r = pread(in_fd, buf + got, len - got, off + (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(in_fd);
    if (got > 0)
        rc = dyn_aio_send(a, out_fd, buf, got, 0, cb, udata);
    else if (cb)
        cb(a, 0, NULL, 0, udata);
    free(buf);
    return rc;
}

int dyn_aio_close(dyn_aio_t *a, int fd)
{
    if (fd >= 0 && fd < a->cap) {
        uaio_fd_t *s = &a->fds[fd];
        if (s->r_op && a->inflight) a->inflight--;
        memset(s, 0, sizeof(*s));
    }
    close(fd); /* cancels outstanding multishot recv/accept on this fd */
    return 0;
}

int dyn_aio_pool_register(dyn_aio_t *a, unsigned n, unsigned sz)
{ (void)a; (void)n; (void)sz; return 0; /* provided-buffer ring is set up in _new */ }
int dyn_aio_openat(dyn_aio_t *a, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata)
{ (void)a;(void)dirfd;(void)path;(void)flags;(void)mode;(void)cb;(void)udata; errno = ENOSYS; return -1; }
int dyn_aio_read(dyn_aio_t *a, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata)
{ (void)a;(void)fd;(void)buf;(void)len;(void)off;(void)cb;(void)udata; errno = ENOSYS; return -1; }
int dyn_aio_write(dyn_aio_t *a, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata)
{ (void)a;(void)fd;(void)buf;(void)len;(void)off;(void)cb;(void)udata; errno = ENOSYS; return -1; }
int dyn_aio_fsync(dyn_aio_t *a, int fd, int datasync, dyn_aio_cb cb, void *ud)
{ (void)a;(void)fd;(void)datasync;(void)cb;(void)ud; errno = ENOSYS; return -1; }
int dyn_aio_cancel(dyn_aio_t *a, dyn_aio_cb cb, void *udata)
{ (void)a;(void)cb;(void)udata; errno = ENOSYS; return -1; }

#endif /* CONFIG_NATIVE_MODULES && CONFIG_IO_URING && __linux__ */
