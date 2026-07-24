/*
 * dyna-aio -- the async IO adapter (see dyna-aio.h).
 *
 * This translation unit currently implements the READINESS backend (kqueue on
 * macOS/BSD, epoll on Linux) via dyn_evloop, presenting the completion-model
 * surface: the adapter hides "wait for readiness, then do the syscall" behind a
 * completion callback, reading/writing directly into the caller's buffer -- no
 * extra copy versus the raw OS call. The io_uring backend (Linux true-completion,
 * send_zc/provided-buffers/splice) and the macOS disk thread pool slot in behind
 * this same interface; those entry points return -1/ENOSYS until landed.
 */
#include "dyna-aio.h"

/* The io_uring backend (dyna-aio-uring.c) replaces this readiness backend when
 * CONFIG_IO_URING is set on Linux; compile this file out then to avoid a clash. */
#if defined(CONFIG_NATIVE_MODULES) && !(defined(CONFIG_IO_URING) && defined(__linux__))

#include "dyna-evloop.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

#ifdef MSG_NOSIGNAL
#define AIO_SEND_FLAGS MSG_NOSIGNAL
#else
#define AIO_SEND_FLAGS 0 /* macOS: SO_NOSIGPIPE is set on the fd instead */
#endif

#define AIO_DEFAULT_BUFSZ 65536u

/* Per-fd op state. A connection can have a read side (accept or recv) and a
 * write side (send) outstanding at once, so both are tracked. */
typedef struct {
    /* read side */
    dyn_aio_cb r_cb;
    void *r_udata;
    uint8_t r_op;        /* AIO_OP_* */
    uint8_t r_multishot; /* keep the read armed after each completion */
    /* write side: a buffered send, optionally followed by a file (sendfile) */
    dyn_aio_cb w_cb;
    void *w_udata;
    const uint8_t *w_buf;
    size_t w_len, w_off;
    uint8_t w_own; /* free(w_buf) when the send completes */
    uint8_t active; /* slot in use */
    /* zero-copy file body (sendfile), sent after any buffered prefix drains */
    int w_file_fd;      /* -1/0 when none; valid iff w_file_rem > 0 */
    off_t w_file_off, w_file_rem;
    dyn_aio_cb w_file_cb;
    void *w_file_udata;
} aio_fd_t;

enum { AIO_OP_NONE = 0, AIO_OP_ACCEPT, AIO_OP_RECV };

struct dyn_aio {
    dyn_evloop_t *lp;
    aio_fd_t *fds;
    int cap;
    size_t inflight; /* armed read/write sides */
    /* ONE shared recv buffer: the readiness backend recvs synchronously inside
     * a callback and dispatches fds one at a time, so a single buffer is reused
     * across every connection (borrowed to the callback, valid until it returns)
     * -- no per-connection recv allocation. */
    uint8_t *rbuf;
    unsigned rcap;
};

static int fd_ensure(dyn_aio_t *a, int fd)
{
    int nc;
    aio_fd_t *nf;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 64;
    while (nc <= fd)
        nc *= 2;
    nf = (aio_fd_t *)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf)
        return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

/* Recompute the reactor interest mask for `fd` from its armed sides. */
static void aio_apply_interest(dyn_aio_t *a, int fd)
{
    aio_fd_t *s = &a->fds[fd];
    int mask = 0;
    if (s->r_op != AIO_OP_NONE)
        mask |= DYN_EV_READ;
    if (s->w_cb || s->w_len > s->w_off || s->w_file_rem > 0)
        mask |= DYN_EV_WRITE;
    dyn_evloop_mod(a->lp, fd, mask);
}

static void aio_read_done(dyn_aio_t *a, int fd)
{
    aio_fd_t *s = &a->fds[fd];
    s->r_op = AIO_OP_NONE;
    if (a->inflight)
        a->inflight--;
}

/* Send as much of the file as the socket accepts; advance the offset/remaining.
 * Returns 0 (progress, possibly EAGAIN with bytes left) or -1 (hard error). */
static int aio_sendfile_step(int sock, int file, off_t *off, off_t *rem)
{
#if defined(__APPLE__)
    while (*rem > 0) {
        off_t n = *rem;
        int r = sendfile(file, sock, *off, &n, NULL, 0);
        *off += n; *rem -= n;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return 0;
        return -1;
    }
    return 0;
#elif defined(__linux__)
    while (*rem > 0) {
        ssize_t n = sendfile(sock, file, off, (size_t)*rem); /* updates *off */
        if (n > 0) { *rem -= n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    return 0;
#else
    (void)sock; (void)file; (void)off; (void)rem; errno = ENOSYS; return -1;
#endif
}

/* Finish a file send on `fd`: close the file, clear state, fire the cb. */
static void aio_file_done(dyn_aio_t *a, int fd, int result)
{
    aio_fd_t *s = &a->fds[fd];
    dyn_aio_cb cb = s->w_file_cb;
    void *ud = s->w_file_udata;
    if (s->w_file_fd > 0) close(s->w_file_fd);
    s->w_file_fd = 0; s->w_file_rem = 0; s->w_file_off = 0;
    s->w_file_cb = NULL; s->w_file_udata = NULL;
    if (a->inflight) a->inflight--;
    aio_apply_interest(a, fd);
    if (cb) cb(a, result, NULL, 0, ud);
}

/* The single dyn_evloop callback: adapt readiness into completions. */
static void aio_dispatch(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_aio_t *a = (dyn_aio_t *)udata;
    aio_fd_t *s = &a->fds[fd];
    (void)lp;

    if ((events & DYN_EV_WRITE) && (s->w_cb || s->w_len > s->w_off)) {
        for (;;) {
            ssize_t n;
            if (s->w_off >= s->w_len) { /* fully sent */
                dyn_aio_cb cb = s->w_cb;
                void *ud = s->w_udata;
                size_t sent = s->w_len;
                if (s->w_own)
                    free((void *)s->w_buf);
                s->w_cb = NULL; s->w_udata = NULL; s->w_buf = NULL;
                s->w_len = s->w_off = 0; s->w_own = 0;
                if (a->inflight) a->inflight--;
                aio_apply_interest(a, fd);
                if (cb) cb(a, (int)sent, NULL, 0, ud);
                break;
            }
            n = send(fd, s->w_buf + s->w_off, s->w_len - s->w_off, AIO_SEND_FLAGS);
            if (n > 0) { s->w_off += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            { /* hard error */
                dyn_aio_cb cb = s->w_cb; void *ud = s->w_udata;
                if (s->w_own) free((void *)s->w_buf);
                s->w_cb = NULL; s->w_buf = NULL; s->w_len = s->w_off = 0; s->w_own = 0;
                if (a->inflight) a->inflight--;
                aio_apply_interest(a, fd);
                if (cb) cb(a, -errno, NULL, 0, ud);
            }
            break;
        }
    }

    /* zero-copy file body: stream once the buffered prefix has drained. Re-fetch
     * `s` in case a completion callback above grew a->fds. */
    s = &a->fds[fd];
    if ((events & DYN_EV_WRITE) && s->w_file_rem > 0 && s->w_len <= s->w_off) {
        if (aio_sendfile_step(fd, s->w_file_fd, &s->w_file_off, &s->w_file_rem) < 0)
            aio_file_done(a, fd, -errno);
        else if (s->w_file_rem == 0)
            aio_file_done(a, fd, 0);
        /* else partial: still armed for DYN_EV_WRITE */
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_ACCEPT) {
        /* Capture cb/udata into locals: the callback (dyn_aio_recv on the new fd)
         * can grow a->fds via realloc, dangling `s` -- never deref it in the loop. */
        dyn_aio_cb acb = s->r_cb;
        void *aud = s->r_udata;
        for (;;) {
            int c = accept(fd, NULL, NULL);
            if (c < 0) {
                if (errno == EINTR) continue;
                break; /* EAGAIN: drained the backlog */
            }
            dyn_net_set_nonblock(c);
            dyn_net_set_nodelay(c);
#ifdef SO_NOSIGPIPE
            { int on = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
            if (acb) acb(a, c, NULL, 0, aud);
        }
        return;
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_RECV) {
        ssize_t n;
        dyn_aio_cb cb = s->r_cb;
        void *ud = s->r_udata;
        do { n = recv(fd, a->rbuf, a->rcap, 0); } while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; /* spurious: stay armed */
        if (!s->r_multishot) {
            aio_read_done(a, fd);
            aio_apply_interest(a, fd);
        }
        if (n >= 0)
            cb(a, (int)n, a->rbuf, (unsigned)n, ud);
        else
            cb(a, -errno, NULL, 0, ud);
    }
}

/* ---- lifecycle -------------------------------------------------------- */

dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t *a = (dyn_aio_t *)calloc(1, sizeof(*a));
    (void)entries; (void)disk_workers; /* used by the io_uring/disk-pool backend */
    if (!a)
        return NULL;
    a->lp = dyn_evloop_new();
    a->rcap = AIO_DEFAULT_BUFSZ;
    a->rbuf = (uint8_t *)malloc(a->rcap);
    if (!a->lp || !a->rbuf) {
        if (a->lp) dyn_evloop_free(a->lp);
        free(a->rbuf);
        free(a);
        return NULL;
    }
    return a;
}

void dyn_aio_free(dyn_aio_t *a)
{
    int fd;
    if (!a)
        return;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t *s = &a->fds[fd];
        if (s->w_own && s->w_buf) free((void *)s->w_buf);
    }
    dyn_evloop_free(a->lp);
    free(a->rbuf);
    free(a->fds);
    free(a);
}

int dyn_aio_backend_fd(const dyn_aio_t *a)
{
    return dyn_evloop_backend_fd(a->lp);
}

void dyn_aio_drain(void *aio)
{
    dyn_aio_t *a = (dyn_aio_t *)aio;
    dyn_evloop_poll(a->lp, 0);
}

int dyn_aio_run(dyn_aio_t *a, int timeout_ms)
{
    return dyn_evloop_poll(a->lp, timeout_ms);
}

size_t dyn_aio_inflight(const dyn_aio_t *a)
{
    return a->inflight;
}

/* ---- network ---------------------------------------------------------- */

int dyn_aio_listen(dyn_aio_t *a, const char *host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 1024) < 0) {
        close(fd);
        return -1;
    }
    dyn_net_set_nonblock(fd);
    return fd;
}

int dyn_aio_accept(dyn_aio_t *a, int listen_fd, dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    if (fd_ensure(a, listen_fd) < 0)
        return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_ACCEPT;
    s->r_multishot = 1;
    a->inflight++;
    if (dyn_evloop_add(a->lp, listen_fd, DYN_EV_READ, aio_dispatch, a) < 0) {
        s->r_op = AIO_OP_NONE;
        a->inflight--;
        return -1;
    }
    return 0;
}

int dyn_aio_recv(dyn_aio_t *a, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    (void)pool; /* provided-buffer pool is an io_uring optimization; shared buf here */
    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_RECV;
    s->r_multishot = multishot ? 1 : 0;
    a->inflight++;
    if (!s->active) {
        s->active = 1;
        if (dyn_evloop_add(a->lp, fd, DYN_EV_READ, aio_dispatch, a) < 0) {
            s->r_op = AIO_OP_NONE; a->inflight--; return -1;
        }
    } else {
        aio_apply_interest(a, fd);
    }
    return 0;
}

int dyn_aio_send(dyn_aio_t *a, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    size_t off = 0;
    (void)flags; /* DYN_AIO_ZC is an io_uring send_zc option; plain send here */

    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    /* fast path: try to send inline; most small responses complete now */
    for (;;) {
        ssize_t n = send(fd, (const uint8_t *)buf + off, len - off, AIO_SEND_FLAGS);
        if (n > 0) { off += (size_t)n; if (off >= len) break; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1; /* hard error */
    }
    if (off >= len) {
        if (cb) cb(a, (int)len, NULL, 0, udata); /* synchronous completion */
        return 0;
    }
    /* partial: buffer the remainder and finish on WRITE readiness */
    {
        uint8_t *copy = (uint8_t *)malloc(len - off);
        if (!copy)
            return -1;
        memcpy(copy, (const uint8_t *)buf + off, len - off);
        s->w_buf = copy; s->w_len = len - off; s->w_off = 0; s->w_own = 1;
        s->w_cb = cb; s->w_udata = udata;
        a->inflight++;
        if (!s->active) {
            s->active = 1;
            if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
                return -1;
        } else {
            aio_apply_interest(a, fd);
        }
    }
    return 0;
}

int dyn_aio_close(dyn_aio_t *a, int fd)
{
    aio_fd_t *s;
    if (fd < 0 || fd >= a->cap)
        { close(fd); return 0; }
    s = &a->fds[fd];
    dyn_evloop_del(a->lp, fd);
    if (s->w_own && s->w_buf) free((void *)s->w_buf);
    if (s->w_file_rem > 0) { /* file transfer interrupted (peer gone): no leak */
        if (s->w_file_fd > 0) close(s->w_file_fd);
        if (a->inflight) a->inflight--;
    }
    if (s->r_op != AIO_OP_NONE && a->inflight) a->inflight--;
    if ((s->w_cb || s->w_len > s->w_off) && a->inflight) a->inflight--;
    memset(s, 0, sizeof(*s));
    close(fd);
    return 0;
}

/* ---- not-yet-landed entry points (io_uring / disk pool increment) ----- */

int dyn_aio_pool_register(dyn_aio_t *a, unsigned n, unsigned sz)
{ (void)a; (void)n; (void)sz; errno = ENOSYS; return -1; }
int dyn_aio_sendfile(dyn_aio_t *a, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    if (fd_ensure(a, out_fd) < 0)
        return -1;
    s = &a->fds[out_fd];
    s->w_file_fd = in_fd; /* adapter owns it: closed on completion/close */
    s->w_file_off = offset;
    s->w_file_rem = (off_t)len;
    s->w_file_cb = cb;
    s->w_file_udata = udata;
    a->inflight++;
    /* if a buffered prefix (e.g. the HTTP header) is still draining, the file is
     * sent after it by the WRITE dispatch; otherwise try to send inline now. */
    if (s->w_len <= s->w_off) {
        if (aio_sendfile_step(out_fd, in_fd, &s->w_file_off, &s->w_file_rem) < 0) {
            aio_file_done(a, out_fd, -errno);
            return 0;
        }
        if (s->w_file_rem == 0) {
            aio_file_done(a, out_fd, 0);
            return 0;
        }
    }
    if (!s->active) {
        s->active = 1;
        if (dyn_evloop_add(a->lp, out_fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
            return -1;
    } else {
        aio_apply_interest(a, out_fd);
    }
    return 0;
}
int dyn_aio_openat(dyn_aio_t *a, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata)
{ (void)a;(void)dirfd;(void)path;(void)flags;(void)mode;(void)cb;(void)udata;
  errno = ENOSYS; return -1; }
int dyn_aio_read(dyn_aio_t *a, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata)
{ (void)a;(void)fd;(void)buf;(void)len;(void)off;(void)cb;(void)udata;
  errno = ENOSYS; return -1; }
int dyn_aio_write(dyn_aio_t *a, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata)
{ (void)a;(void)fd;(void)buf;(void)len;(void)off;(void)cb;(void)udata;
  errno = ENOSYS; return -1; }
int dyn_aio_fsync(dyn_aio_t *a, int fd, int datasync, dyn_aio_cb cb, void *ud)
{ (void)a;(void)fd;(void)datasync;(void)cb;(void)ud; errno = ENOSYS; return -1; }
int dyn_aio_cancel(dyn_aio_t *a, dyn_aio_cb cb, void *udata)
{ (void)a;(void)cb;(void)udata; errno = ENOSYS; return -1; }

#endif /* CONFIG_NATIVE_MODULES */
