/* Opt-in GPU SHARE transport. Its memory is separate from disk DMA and RAM. */
#ifndef TARGET_GPU_H
#define TARGET_GPU_H
#include "../graphics/host/vgpu-host.h"
_Static_assert(VGPU_COMMAND_BASE >= FB_GPA + FB_SIZE, "GPU overlaps scanout");
_Static_assert(VGPU_POOL_BASE == VGPU_COMMAND_BASE + VGPU_COMMAND_SIZE, "GPU layout gap");
_Static_assert(VGPU_COMMAND_BASE >= UINT64_C(0x40000000), "GPU overlaps GIC");
_Static_assert(VGPU_COMMAND_BASE >= GPA + RAM_SIZE, "GPU overlaps private RAM");
_Static_assert(VGPU_POOL_BASE <= UINT64_MAX - VGPU_POOL_SIZE, "GPU address overflow");
struct target_gpu { int enabled; struct vgpu_host host; };
static struct target_gpu gpu;

static int target_gpu_prepare(struct target_gpu *g, int directory) {
    int fd = openat(directory, "gpu.config", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st; char config[sizeof(VGPU_CONFIG)] = {0};
    int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof(VGPU_CONFIG) - 1 &&
                read(fd, config, sizeof config) == sizeof(VGPU_CONFIG) - 1 &&
                !memcmp(config, VGPU_CONFIG, sizeof(VGPU_CONFIG) - 1);
    close(fd);
    if (!valid) { errno = EINVAL; return -1; }
    char executable[128];
    int n = snprintf(executable, sizeof executable, "/proc/self/fd/%d/gpu-backend", directory);
    if (n <= 0 || (size_t)n >= sizeof executable) { errno = ENAMETOOLONG; return -1; }
    g->enabled = 1;
    if (vgpu_host_prepare(&g->host, executable, 1)) return -1;
    puts("GPU_SHARED_RESERVED backend_started=0");
    return 0;
}
static int target_gpu_activate(struct target_gpu *g) {
    if (!g->enabled) return 0;
    if (vgpu_host_activate(&g->host)) return -1;
    printf("GPU_BACKEND_READY pid=%d commands=0x%" PRIx64 " pool=0x%" PRIx64 " bytes=%u polling=1\n",
           g->host.child, VGPU_COMMAND_BASE, VGPU_POOL_BASE, VGPU_MEMORY_SIZE);
    return 0;
}
static int target_gpu_mmio(uint64_t address, unsigned length, unsigned char *data, unsigned write) {
    if (!gpu.enabled || address < VGPU_MMIO_BASE || address >= VGPU_MMIO_BASE + 0x1000 ||
        length != 4 || (address & 3) || write > 1 || !data) { errno = EINVAL; return -1; }
    uint32_t value = 0;
    if (write) memcpy(&value, data, 4);
    if (vgpu_host_mmio(&gpu.host, (uint32_t)(address - VGPU_MMIO_BASE), length, write, &value)) {
        perror("GPU IPC failure; retaining VM owner"); return -1;
    }
    if (!write) memcpy(data, &value, 4);
    return 0;
}
static int target_gpu_release(struct target_gpu *g) {
    if (!g->enabled) return 0;
    int rc = vgpu_host_release(&g->host);
    if (!g->host.child) g->enabled = 0;
    return rc;
}
#endif
