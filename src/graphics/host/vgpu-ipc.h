/* Private monitor/backend protocol. No guest pointers or VM fds cross IPC. */
#ifndef OPENOOS_VGPU_IPC_H
#define OPENOOS_VGPU_IPC_H
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define VGPU_MMIO_BASE UINT64_C(0x10005000)
#define VGPU_COMMAND_BASE UINT64_C(0x200000000)
#define VGPU_COMMAND_SIZE (16u << 20)
#define VGPU_POOL_BASE UINT64_C(0x201000000)
#define VGPU_POOL_SIZE (1536u << 20)
#define VGPU_MEMORY_SIZE (VGPU_COMMAND_SIZE + VGPU_POOL_SIZE)
#define VGPU_IPC_MAGIC UINT32_C(0x55475056)
#define VGPU_CONFIG "openoos-vgpu-v1 commands=0x200000000:0x1000000 pool=0x201000000:0x60000000 mmio=0x10005000 polling=1\n"
enum { VGPU_IPC_HELLO = 1, VGPU_IPC_MMIO = 2, VGPU_IPC_STOP = 3 };
struct vgpu_packet {
    uint32_t magic, version, operation, sequence, offset, width, write, value;
    int32_t result;
    uint32_t reserved;
};
_Static_assert(sizeof(struct vgpu_packet) == 40, "GPU IPC layout");
static inline int64_t vgpu_now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static inline int vgpu_wait_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - vgpu_now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, left > 30000 ? 30000 : (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (!rc) errno = ETIMEDOUT; return -1; }
        if (p.revents & events) return 0;
        errno = EPIPE; return -1;
    }
}
static inline int vgpu_packet_send(int fd, const struct vgpu_packet *p, int64_t deadline) {
    for (;;) {
        if (vgpu_wait_fd(fd, POLLOUT, deadline)) return -1;
        ssize_t n = send(fd, p, sizeof *p, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n == sizeof *p) return 0;
        if (n >= 0) errno = EPROTO;
        return -1;
    }
}
static inline int vgpu_packet_recv(int fd, struct vgpu_packet *p, int64_t deadline) {
    for (;;) {
        if (vgpu_wait_fd(fd, POLLIN, deadline)) return -1;
        /* MSG_TRUNC returns the complete datagram length: oversized packets
         * cannot be silently accepted as a valid prefix. */
        ssize_t n = recv(fd, p, sizeof *p, MSG_TRUNC | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n == sizeof *p) return 0;
        errno = n == 0 ? EPIPE : EPROTO;
        return -1;
    }
}
static inline int vgpu_request_valid(const struct vgpu_packet *p, uint32_t sequence) {
    if (p->magic != VGPU_IPC_MAGIC || p->version != 1 || p->sequence != sequence ||
        !sequence || p->result || p->reserved) return 0;
    if (p->operation == VGPU_IPC_MMIO)
        return sequence > 1 && p->width == 4 && p->offset < 0x1000 &&
               !(p->offset & 3) && p->write <= 1 && (p->write || !p->value);
    if (p->operation != (sequence == 1 ? VGPU_IPC_HELLO : VGPU_IPC_STOP)) return 0;
    return !(p->offset | p->width | p->write | p->value);
}
#endif
