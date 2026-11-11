/* Root-side nonblocking connector. Receives only the app's publication memfd. */
#ifndef SCANOUT_CLIENT_H
#define SCANOUT_CLIENT_H
#include "scanout-ring.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
struct scanout_client {
    int socket; struct scanout_ring *ring; double attempted, connected, last_frame;
    uint64_t sequence, sent, dropped, sampled, unchanged; int previous;
};
static inline void scanout_disconnect(struct scanout_client *c) {
    if (c->ring) munmap(c->ring, SCANOUT_SIZE);
    if (c->socket >= 0) close(c->socket);
    c->socket = -1; c->ring = NULL; c->previous = -1;
}
static inline int scanout_connect(struct scanout_client *c, const char *name, unsigned uid, double now) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    size_t len = strlen(name);
    if (len < 16 || len >= sizeof(a.sun_path)-1) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < len; ++i)
        if (!(name[i] == '-' || (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9'))) {
            errno = EINVAL; return -1;
        }
    memcpy(a.sun_path + 1, name, len);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    struct ucred peer; socklen_t size = sizeof(peer);
    if (connect(s, (void *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path)+len+1)) ||
        getsockopt(s, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != uid) {
        close(s); return -1;
    }
    c->socket = s; c->connected = now;
    return 0;
}
static inline int scanout_receive(struct scanout_client *c, double now) {
    if (c->socket < 0) return 0;
    if (c->ring) {
        char b; ssize_t n = recv(c->socket, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        scanout_disconnect(c); return 0;
    }
    char byte = 0; union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int)*4)]; } control;
    struct iovec io = {.iov_base = &byte, .iov_len = 1};
    struct msghdr msg = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control.data, .msg_controllen = sizeof(control)};
    ssize_t n = recvmsg(c->socket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && now-c->connected < 2) return 0;
    int fd = -1, count = 0;
    if (n > 0) for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS && h->cmsg_len >= CMSG_LEN(0)) {
            size_t nf = (h->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for (size_t i = 0; i < nf; ++i) { int got; memcpy(&got, CMSG_DATA(h)+i*sizeof(int), sizeof(got));
                if (count++ == 0) fd = got; else close(got); }
        }
    struct stat st;
    const int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
    int seals = fd >= 0 ? fcntl(fd, F_GET_SEALS) : -1;
    if (n != 1 || byte != 2 || count != 1 || (msg.msg_flags & MSG_CTRUNC) || fd < 0 ||
        fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != SCANOUT_SIZE ||
        seals < 0 || (seals & required) != required) goto fail;
    void *mapping = mmap(NULL, SCANOUT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); fd = -1;
    if (mapping == MAP_FAILED) goto fail;
    c->ring = mapping; c->previous = -1;
    if (c->ring->magic != SCANOUT_MAGIC || c->ring->version != 2 || c->ring->bytes != SCANOUT_BYTES) goto fail;
    puts("SCANOUT_STREAM_CONNECTED version=2 publication_only=1"); fflush(stdout);
    return 1;
fail:
    if (fd >= 0) close(fd);
    scanout_disconnect(c); return 0;
}
static inline int scanout_send(struct scanout_client *c, const void *pixels, double now) {
    if (!c->ring || now-c->last_frame < 1.0/120.0) return 0;
    /* One cadence gate for all producers. Preserve phase across coarse polls;
     * resetting to now rounds 8.33 ms up to the controller's next tick.
     * Skip missed slots after stalls instead of publishing a catch-up burst. */
    c->last_frame += 1.0/120.0;
    if (now-c->last_frame >= 1.0/120.0) c->last_frame = now;
    ++c->sampled;
    if (__atomic_exchange_n(&c->ring->reserved, 0, __ATOMIC_ACQ_REL)) c->previous = -1;
    /* Only producer changes pixels; reader holds slots without modifying them. */
    if (c->previous >= 0 && !memcmp(scanout_pixels(c->ring, (unsigned)c->previous), pixels, SCANOUT_BYTES)) {
        ++c->unchanged; return 0;
    }
    int i = scanout_claim(c->ring);
    if (i < 0) { ++c->dropped; return 0; }
    scanout_publish(c->ring, (unsigned)i, pixels, ++c->sequence, (uint64_t)(now*1e9));
    c->previous = i; ++c->sent;
    return 1;
}
#endif
