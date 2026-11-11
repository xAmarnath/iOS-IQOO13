#define _GNU_SOURCE
#include "vgpu-host.h"
#include <assert.h>

/* Executed child is an explicit IPC test peer; no GPU emulation in production. */
static int peer(void) {
    assert(fcntl(9, F_GETFD) < 0 && errno == EBADF);
    unsigned char *memory = mmap(NULL, VGPU_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    assert(memory != MAP_FAILED);
    const char *mode = getenv("VGPU_TEST_FAULT");
    uint32_t sequence = 0;
    for (;;) {
        struct vgpu_packet p;
        assert(!vgpu_packet_recv(3, &p, vgpu_now_ms() + 2000));
        assert(vgpu_request_valid(&p, ++sequence));
        if (mode && sequence == 2) {
            if (!strcmp(mode, "eof")) break;
            if (!strcmp(mode, "stale")) --p.sequence;
        }
        if (p.operation == VGPU_IPC_MMIO) {
            if (p.write) memcpy(memory + 64, &p.value, 4);
            else memcpy(&p.value, memory + 64, 4);
        }
        assert(!vgpu_packet_send(3, &p, vgpu_now_ms() + 2000));
        if (p.operation == VGPU_IPC_STOP && mode && !strcmp(mode, "retained")) {
            int64_t deadline = vgpu_now_ms() + 6000;
            while (!__atomic_load_n((uint32_t *)(memory + 128), __ATOMIC_ACQUIRE)) {
                assert(vgpu_now_ms() < deadline);
                struct timespec tick = {.tv_nsec = 10000000}; nanosleep(&tick, NULL);
            }
        }
        if (p.operation == VGPU_IPC_STOP || (mode && sequence == 2)) break;
    }
    munmap(memory, VGPU_MEMORY_SIZE);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3) return peer();
    assert(argc == 1);
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    struct vgpu_packet p = {.magic = VGPU_IPC_MAGIC, .version = 1,
                            .operation = VGPU_IPC_HELLO, .sequence = 1};
    assert(vgpu_request_valid(&p, 1));
    for (unsigned i = 0; i < sizeof p; ++i) {
        struct vgpu_packet bad = p;
        ((unsigned char *)&bad)[i] ^= 0x80;
        assert(!vgpu_request_valid(&bad, 1));
    }
    unsigned char oversized[sizeof p + 1] = {0}; memcpy(oversized, &p, sizeof p);
    assert(send(pair[0], oversized, sizeof oversized, 0) == sizeof oversized);
    assert(vgpu_packet_recv(pair[1], &p, vgpu_now_ms() + 100) < 0 && errno == EPROTO);
    assert(send(pair[0], &p, 4, 0) == 4);
    assert(vgpu_packet_recv(pair[1], &p, vgpu_now_ms() + 100) < 0 && errno == EPROTO);
    assert(vgpu_packet_recv(pair[1], &p, vgpu_now_ms() + 20) < 0 && errno == ETIMEDOUT);
    close(pair[0]);
    assert(vgpu_packet_send(pair[1], &p, vgpu_now_ms() + 100) < 0); /* no SIGPIPE */
    close(pair[1]);
    char *path = realpath(argv[0], NULL); assert(path);
    int leak = open("/dev/null", O_RDONLY); assert(leak >= 0);
    assert(dup2(leak, 9) == 9); if (leak != 9) close(leak);
    struct vgpu_host host;
    assert(!vgpu_host_start(&host, path, 0));
    uint32_t value = 0xfeed1234;
    assert(!vgpu_host_mmio(&host, 0, 4, 1, &value));
    assert(!memcmp(host.shared + 64, &value, 4));
    value = 0;
    assert(!vgpu_host_mmio(&host, 0, 4, 0, &value) && value == 0xfeed1234);
    assert(!vgpu_host_release(&host));
    for (unsigned i = 0; i < 2; ++i) {
        assert(!setenv("VGPU_TEST_FAULT", i ? "stale" : "eof", 1));
        assert(!vgpu_host_start(&host, path, 0));
        assert(vgpu_host_mmio(&host, 0, 4, 0, &value) < 0 && host.failed);
        uint32_t sequence = host.sequence;
        assert(vgpu_host_mmio(&host, 0, 4, 0, &value) < 0 && host.sequence == sequence);
        assert(!vgpu_host_release(&host));
    }
    assert(!setenv("VGPU_TEST_FAULT", "retained", 1));
    assert(!vgpu_host_start(&host, path, 0));
    pid_t child = host.child; void *mapping = host.shared; int fd = host.memory;
    assert(vgpu_host_release(&host) < 0 && errno == EBUSY);
    assert(host.child == child && host.shared == mapping && host.memory == fd);
    assert(!kill(child, 0) && fcntl(fd, F_GETFD) >= 0);
    __atomic_store_n((uint32_t *)(host.shared + 128), 1, __ATOMIC_RELEASE);
    assert(!vgpu_host_release(&host));
    unsetenv("VGPU_TEST_FAULT"); close(9); free(path);
    puts("IPC PASS: bounds, corrupt headers, truncated packets, timeout, EOF, stale replies, fd isolation, shared visibility, cleanup, retained-child recovery");
}
