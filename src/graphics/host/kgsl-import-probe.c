/* Own allocations only; no VM memory or GPU command submission. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
#include "drm2kgsl/msm_kgsl.h"

static unsigned failures;

static void probe(const char *kind, int buffer)
{
    const uint64_t flags[] = {0, KGSL_MEMFLAGS_IOCOHERENT,
        (KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT) | KGSL_MEMFLAGS_IOCOHERENT};
    for (unsigned i = 0; i < sizeof(flags)/sizeof(flags[0]); i++) {
        int gpu = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
        if (gpu < 0) { perror("kgsl"); exit(1); }
        struct kgsl_gpuobj_import_dma_buf p = {.fd = buffer};
        struct kgsl_gpuobj_import req = {.priv = (uintptr_t)&p, .priv_len = sizeof(p),
            .flags = flags[i], .type = KGSL_USER_MEM_TYPE_DMABUF};
        errno = 0;
        int rc = ioctl(gpu, IOCTL_KGSL_GPUOBJ_IMPORT, &req), error = errno;
        printf("IMPORT kind=%s flags=0x%" PRIx64 " rc=%d errno=%d id=%u\n",
               kind, flags[i], rc, error, req.id);
        if (rc) failures++;
        if (!rc) {
            struct kgsl_gpuobj_free release = {.id = req.id};
            if (ioctl(gpu, IOCTL_KGSL_GPUOBJ_FREE, &release)) { perror("free"); exit(1); }
        }
        close(gpu);
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    alarm(20);
    const size_t size = 65536, offset = 4u << 20, arena_size = 16u << 20;
    int keeper = -1;
    struct kgsl_gpuobj_alloc reservation = {0};
    if (argc > 1) {
        char *end = NULL;
        unsigned long gib = strtoul(argv[1], &end, 10);
        if (!end || *end || !gib || gib > 256) return 2;
        keeper = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
        reservation.size = (uint64_t)gib << 30;
        reservation.flags = KGSL_MEMFLAGS_VBO;
        if (keeper < 0 || ioctl(keeper, IOCTL_KGSL_GPUOBJ_ALLOC, &reservation))
            return perror("vbo-reserve"), 1;
        struct kgsl_gpuobj_info info = {.id = reservation.id};
        if (ioctl(keeper, IOCTL_KGSL_GPUOBJ_INFO, &info)) return perror("vbo-info"), 1;
        printf("VBO gib=%lu id=%u base=0x%" PRIx64 "\n", gib, reservation.id, (uint64_t)info.gpuaddr);
    }
    int mem = memfd_create("gpu-import-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mem < 0 || ftruncate(mem, arena_size) || fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW))
        return perror("memfd"), 1;
    void *map = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    if (map == MAP_FAILED) return perror("mmap"), 1;
    memset((char *)map + offset, 0, size);
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    struct udmabuf_create create = {.memfd = mem, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = offset, .size = size};
    int buf = dev < 0 ? -1 : ioctl(dev, UDMABUF_CREATE, &create);
    if (buf < 0) return perror("udmabuf"), 1;
    close(dev);
    probe("udmabuf", buf);
    close(buf);
    munmap(map, arena_size);
    close(mem);
    const char *heaps[] = {"/dev/dma_heap/system", "/dev/dma_heap/qcom,system"};
    for (unsigned i = 0; i < sizeof(heaps)/sizeof(heaps[0]); i++) {
        dev = open(heaps[i], O_RDWR | O_CLOEXEC);
        struct dma_heap_allocation_data a = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
        if (dev < 0 || ioctl(dev, DMA_HEAP_IOCTL_ALLOC, &a)) {
            printf("ALLOC kind=%s errno=%d\n", heaps[i], errno);
            if (dev >= 0) close(dev);
            continue;
        }
        close(dev);
        probe(heaps[i], a.fd);
        close(a.fd);
    }
    if (keeper >= 0) {
        struct kgsl_gpuobj_free release = {.id = reservation.id};
        if (ioctl(keeper, IOCTL_KGSL_GPUOBJ_FREE, &release)) return perror("vbo-free"), 1;
        close(keeper);
    }
    printf("RESULT import_cases=9 failures=%u gpu_submission=false\n", failures);
    return failures ? 1 : 0;
}
