/* Header-only so the static glibc monitor and Android tests share lifecycle code.
 * Start before opening /dev/gunyah. Release ONLY before start or after VM close. */
#ifndef OPENOOS_VGPU_HOST_H
#define OPENOOS_VGPU_HOST_H
#include "vgpu-ipc.h"
#include "vgpu-shared-memory.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct vgpu_host {
    int socket, memory, executable, failed;
    pid_t child;
    unsigned char *shared;
    uint32_t sequence;
};
static inline int vgpu_host_call(struct vgpu_host *h, uint32_t op, uint32_t offset,
                                 unsigned width, unsigned write, uint32_t *value, int timeout) {
    if (h->failed || h->socket < 0 || h->sequence == UINT32_MAX) { errno = EPIPE; return -1; }
    struct vgpu_packet req = {.magic = VGPU_IPC_MAGIC, .version = 1, .operation = op,
        .sequence = ++h->sequence, .offset = offset, .width = width, .write = write,
        .value = write ? *value : 0}, reply;
    int64_t started = vgpu_now_ms(), sent = -1, deadline = started + timeout;
    const char *stage = "send";
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (vgpu_packet_send(h->socket, &req, deadline)) goto fail;
    sent = vgpu_now_ms(); stage = "receive";
    if (vgpu_packet_recv(h->socket, &reply, deadline)) goto fail;
    stage = "validate";
    uint32_t returned = reply.value; int32_t result = reply.result;
    reply.value = req.value; reply.result = 0;
    if (memcmp(&req, &reply, sizeof req) || result > 0) { errno = EPROTO; goto fail; }
    if (result) { errno = EIO; goto fail; }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (value && !write) *value = returned;
    return 0;
fail:
    {
        int saved = errno;
        fprintf(stderr, "GPU_IPC_FAILURE stage=%s op=%u seq=%u offset=0x%x width=%u write=%u "
                "value=0x%x child=%ld timeout_ms=%d send_ms=%lld elapsed_ms=%lld errno=%d\n",
                stage, op, req.sequence, offset, width, write, req.value, (long)h->child,
                timeout, (long long)(sent < 0 ? -1 : sent - started),
                (long long)(vgpu_now_ms() - started), saved);
        h->failed = 1; errno = saved; return -1;
    }
}
static inline void vgpu_child_exec(int socket_fd, int memory_fd, int executable_fd) {
    int a = fcntl(socket_fd, F_DUPFD_CLOEXEC, 10);
    int b = fcntl(memory_fd, F_DUPFD_CLOEXEC, 10);
    int c = fcntl(executable_fd, F_DUPFD_CLOEXEC, 10);
    if (a < 0 || b < 0 || c < 0 || dup2(a, 3) < 0 || dup2(b, 4) < 0 || dup2(c, 5) < 0) _exit(126);
    if (fcntl(5, F_SETFD, FD_CLOEXEC)) _exit(126);
    /* close_range is not exposed by older Android libc; syscall when present. */
#ifdef __NR_close_range
    if (syscall(__NR_close_range, 6u, UINT_MAX, 0))
#endif
    {
        long maximum = sysconf(_SC_OPEN_MAX);
        if (maximum < 0 || maximum > 1048576) _exit(126);
        for (int fd = 6; fd < maximum; ++fd) close(fd);
    }
    alarm(0);
    signal(SIGALRM, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
    signal(SIGUSR1, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGCHLD, SIG_DFL);
    sigset_t empty; sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL)) _exit(126);
    extern char **environ;
    char *argv[] = {"gpu-backend", "3", "4", NULL};
    fexecve(5, argv, environ);
    _exit(127);
}
/* Reserve shared folios without initializing the native driver. Private guest
 * RAM can then be prepared before the driver allocates physical page tables. */
static inline int vgpu_host_prepare(struct vgpu_host *h, const char *executable, int pin) {
    *h = (struct vgpu_host){.socket = -1, .memory = -1, .executable = -1, .shared = MAP_FAILED};
    int exe = h->executable = open(executable, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (exe < 0) return -1;
    if (fstat(exe, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
        errno = EINVAL; return -1;
    }
    h->memory = memfd_create("openoos-vgpu-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (h->memory < 0 || ftruncate(h->memory, VGPU_MEMORY_SIZE) ||
        fcntl(h->memory, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) goto fail;
    if (pin) {
        /* Prefer large folios; fragmented backing retains residency checks. */
        h->shared = vgpu_shared_prepare(h->memory, VGPU_MEMORY_SIZE, 1);
        if (h->shared == MAP_FAILED) goto fail;
    } else {
        /* Unprivileged host lifecycle tests do not have PFN/locking access. */
        h->shared = mmap(NULL, VGPU_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, h->memory, 0);
        if (h->shared == MAP_FAILED || madvise(h->shared, VGPU_MEMORY_SIZE, MADV_NOHUGEPAGE)) goto fail;
        memset(h->shared, 0, VGPU_MEMORY_SIZE);
    }
    return 0;
fail:
    return -1; /* caller releases partial allocations */
}
static inline int vgpu_host_activate(struct vgpu_host *h) {
    if (h->failed || h->child || h->socket >= 0 || h->executable < 0 ||
        h->memory < 0 || h->shared == MAP_FAILED) { errno = EINVAL; return -1; }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return -1;
    h->socket = sockets[0];
    h->child = fork();
    if (!h->child) vgpu_child_exec(sockets[1], h->memory, h->executable);
    int saved = errno;
    close(sockets[1]); close(h->executable); h->executable = -1;
    if (h->child < 0) { h->child = 0; errno = saved; return -1; }
    return vgpu_host_call(h, VGPU_IPC_HELLO, 0, 0, 0, NULL, 30000);
}
static inline int vgpu_host_start(struct vgpu_host *h, const char *executable, int pin) {
    if (vgpu_host_prepare(h, executable, pin)) return -1;
    return vgpu_host_activate(h);
}
static inline int vgpu_host_mmio(struct vgpu_host *h, uint32_t offset, unsigned width,
                                 unsigned write, uint32_t *value) {
    return vgpu_host_call(h, VGPU_IPC_MMIO, offset, width, write, value, 2000);
}
static inline int vgpu_host_release(struct vgpu_host *h) {
    int result = 0;
    if (h->child > 0) {
        if (!h->failed && h->socket >= 0 && vgpu_host_call(h, VGPU_IPC_STOP, 0, 0, 0, NULL, 2000)) result = -1;
        if (h->socket >= 0) { close(h->socket); h->socket = -1; }
        int status = 0, ended = 0;
        int64_t deadline = vgpu_now_ms() + 2000;
        do {
            pid_t p = waitpid(h->child, &status, WNOHANG);
            if (p == h->child) { ended = 1; break; }
            if (p < 0 && errno != EINTR) { ended = 1; result = -1; break; }
            struct timespec t = {.tv_nsec = 10000000}; nanosleep(&t, NULL);
        } while (vgpu_now_ms() < deadline);
        if (!ended) {
            /* Native work may still own shared pages. Keep child and mapping
             * for inspection/recovery; do not turn a timeout into teardown. */
            h->failed = 1; errno = EBUSY;
            return -1;
        } else if (!WIFEXITED(status) || WEXITSTATUS(status)) result = -1;
        h->child = 0;
    }
    if (h->socket >= 0) { close(h->socket); h->socket = -1; }
    if (h->executable >= 0) { close(h->executable); h->executable = -1; }
    if (h->shared != MAP_FAILED) { if (munmap(h->shared, VGPU_MEMORY_SIZE)) result = -1; h->shared = MAP_FAILED; }
    if (h->memory >= 0) { close(h->memory); h->memory = -1; }
    return result;
}
#endif
