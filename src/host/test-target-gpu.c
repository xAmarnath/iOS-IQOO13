#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <errno.h>
#include <stddef.h>
#include "../graphics/host/vgpu-ipc.h"
#include "../graphics/host/vgpu-shared-memory.h"
/* Host test models folio preparation/pinning; IPC/fork/memfd paths are real.
 * Actual folios and native DMA are qualified by the device transport probe. */
static size_t pinned;
static int pin_failure;
static int fixture_mlock(const void *address, size_t length) {
    assert(address && length == VGPU_MEMORY_SIZE); pinned = length;
    if (pin_failure) { errno = ENOMEM; return -1; }
    return 0;
}
static void *fixture_shared_prepare(int fd, size_t length, int pin) {
    assert(pin == 1 && length == VGPU_MEMORY_SIZE);
    void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) return mapping;
    memset(mapping, 0, length);
    if (fixture_mlock(mapping, length)) {
        int saved = errno; munmap(mapping, length); errno = saved;
        return MAP_FAILED;
    }
    return mapping;
}
#define vgpu_shared_prepare fixture_shared_prepare
#define FB_GPA UINT64_C(0x15000000)
#define FB_SIZE (12u << 20)
#define GPA UINT64_C(0x80000000)
#define RAM_SIZE (UINT64_C(5120) << 20)
#include "target-gpu.h"
#undef vgpu_shared_prepare

static int peer(void) {
    assert(fcntl(9, F_GETFD) < 0 && errno == EBADF);
    uint32_t sequence = 0, value = 0;
    unsigned char *memory = mmap(NULL, VGPU_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    assert(memory != MAP_FAILED);
    for (;;) {
        struct vgpu_packet p;
        assert(!vgpu_packet_recv(3, &p, vgpu_now_ms() + 5000));
        assert(vgpu_request_valid(&p, ++sequence));
        if (p.operation == VGPU_IPC_MMIO) {
            if (getenv("VGPU_TEST_EOF")) break;
            if (p.write) value = p.value; else p.value = value;
            memcpy(memory + 64, &value, 4);
        }
        assert(!vgpu_packet_send(3, &p, vgpu_now_ms() + 2000));
        if (p.operation == VGPU_IPC_STOP) break;
    }
    assert(!munmap(memory, VGPU_MEMORY_SIZE));
    return 0;
}
static void config(int directory, const void *data, size_t size) {
    int fd = openat(directory, "gpu.config", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size); close(fd);
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc == 3) return peer();
    assert(argc == 1);
    char path[] = "/tmp/target-gpu-XXXXXX";
    assert(mkdtemp(path));
    int directory = open(path, O_RDONLY | O_DIRECTORY); assert(directory >= 0);
    unsigned char data[4] = {0};
    assert(!target_gpu_prepare(&gpu, directory) && !gpu.enabled);
    assert(target_gpu_mmio(VGPU_MMIO_BASE, 4, data, 0) < 0);
    config(directory, VGPU_CONFIG, sizeof(VGPU_CONFIG)); /* trailing NUL rejected */
    assert(target_gpu_prepare(&gpu, directory) < 0 && !gpu.enabled);
    config(directory, "bad", 3);
    assert(target_gpu_prepare(&gpu, directory) < 0 && !gpu.enabled);
    const char old_config[] = "openoos-vgpu-v1 commands=0x22000000:0x1000000 pool=0x23000000:0x4000000 mmio=0x10005000 polling=1\n";
    config(directory, old_config, sizeof(old_config) - 1);
    assert(target_gpu_prepare(&gpu, directory) < 0 && !gpu.enabled);
    assert(!unlinkat(directory, "gpu.config", 0));
    assert(!symlinkat("/dev/null", directory, "gpu.config"));
    assert(target_gpu_prepare(&gpu, directory) < 0 && !gpu.enabled);
    assert(!unlinkat(directory, "gpu.config", 0));
    config(directory, VGPU_CONFIG, sizeof(VGPU_CONFIG) - 1);
    assert(target_gpu_prepare(&gpu, directory) < 0 && gpu.enabled); /* missing executable */
    assert(!target_gpu_release(&gpu) && !gpu.enabled);
    int source = open("/proc/self/exe", O_RDONLY);
    int target = openat(directory, "gpu-backend", O_WRONLY | O_CREAT | O_EXCL, 0700);
    assert(source >= 0 && target >= 0);
    unsigned char buffer[16384]; ssize_t n;
    while ((n = read(source, buffer, sizeof buffer)) > 0) assert(write(target, buffer, n) == n);
    assert(n == 0); close(source); close(target);
    int leak = open("/dev/null", O_RDONLY); assert(leak >= 0 && dup2(leak, 9) == 9);
    if (leak != 9) close(leak);
    pin_failure = 1;
    assert(target_gpu_prepare(&gpu, directory) < 0 && gpu.enabled && pinned == VGPU_MEMORY_SIZE);
    assert(!target_gpu_release(&gpu)); pin_failure = 0;
    /* Abort between reservation and native startup: no child, all FDs freed. */
    assert(!target_gpu_prepare(&gpu, directory) && gpu.enabled);
    assert(!gpu.host.child && gpu.host.socket == -1 && gpu.host.executable >= 0);
    int reserved_exe = gpu.host.executable, reserved_mem = gpu.host.memory;
    assert(!target_gpu_release(&gpu) && !gpu.enabled);
    assert(fcntl(reserved_exe, F_GETFD) < 0 && errno == EBADF);
    assert(fcntl(reserved_mem, F_GETFD) < 0 && errno == EBADF);
    for (unsigned fault = 0; fault < 2; ++fault) {
        if (fault) assert(!setenv("VGPU_TEST_EOF", "1", 1));
        assert(!target_gpu_prepare(&gpu, directory) && gpu.enabled);
        assert(!gpu.host.child && gpu.host.socket == -1);
        assert(target_gpu_mmio(VGPU_MMIO_BASE, 4, data, 0) < 0);
        assert(!gpu.host.failed && !gpu.host.sequence);
        assert(!target_gpu_activate(&gpu));
        assert(gpu.host.child > 0 && gpu.host.shared != MAP_FAILED);
        assert(target_gpu_activate(&gpu) < 0 && !gpu.host.failed);
        uint32_t before = gpu.host.sequence;
        assert(target_gpu_mmio(VGPU_MMIO_BASE - 4, 4, data, 0) < 0);
        assert(target_gpu_mmio(VGPU_MMIO_BASE + 0x1000, 4, data, 0) < 0);
        assert(target_gpu_mmio(VGPU_MMIO_BASE + 1, 4, data, 0) < 0);
        assert(target_gpu_mmio(VGPU_MMIO_BASE, 8, data, 0) < 0);
        assert(target_gpu_mmio(VGPU_MMIO_BASE, 4, data, 2) < 0);
        assert(target_gpu_mmio(VGPU_MMIO_BASE, 4, NULL, 0) < 0);
        assert(gpu.host.sequence == before);
        if (fault) {
            assert(target_gpu_mmio(VGPU_MMIO_BASE, 4, data, 0) < 0 && gpu.host.failed);
            /* Error must retain memory/backend identity for the VM owner. */
            assert(gpu.enabled && gpu.host.child > 0 && gpu.host.shared != MAP_FAILED);
        } else {
            uint32_t value = 0x87654321; memcpy(data, &value, 4);
            assert(!target_gpu_mmio(VGPU_MMIO_BASE + 0xffc, 4, data, 1));
            memset(data, 0, 4);
            assert(!target_gpu_mmio(VGPU_MMIO_BASE, 4, data, 0));
            assert(!memcmp(data, &value, 4) && !memcmp(gpu.host.shared + 64, &value, 4));
        }
        assert(!target_gpu_release(&gpu) && !gpu.enabled && gpu.host.memory == -1);
        assert(!target_gpu_release(&gpu));
    }
    unsetenv("VGPU_TEST_EOF"); close(9);
    assert(!unlinkat(directory, "gpu.config", 0));
    assert(!unlinkat(directory, "gpu-backend", 0)); close(directory); assert(!rmdir(path));
    puts("GPU_MONITOR_PASS config, pin failure, MMIO bounds, IPC, retained error state, FD isolation, cleanup; folios/pinning modeled");
}
