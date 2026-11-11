/* Real drm2kgsl native-context qualification. No VM operations. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <linux/dma-buf.h>
#include <time.h>
#include <unistd.h>
#include "AdrenoPM4.h"
#include "virgl_context.h"
#include "virglrenderer.h"
#include "drm2kgsl/drm2kgsl_renderer.h"
#include "drm-uapi/msm_drm.h"
#include "msm/msm_proto.h"

static void fail(const char *stage, int code)
{
    fprintf(stderr, "FAIL stage=%s code=%d errno=%d (%s)\n", stage, code, errno, strerror(errno));
    fflush(NULL);
    exit(1);
}

static void env_u64(const char *name, uint64_t value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
    if (setenv(name, buffer, 1)) fail("setenv", -1);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fail("clock", -1);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static void cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    int ret;
    do { ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (ret && errno == EINTR);
    if (ret) fail("dma-buf-cpu-sync", ret);
}

static void simple_ioctl(struct virgl_context *ctx, struct msm_shmem *shmem,
                         void *response, uint32_t *seq, uint32_t cmd, void *arg, size_t len)
{
    unsigned char buffer[sizeof(struct msm_ccmd_ioctl_simple_req) + 64] = {0};
    if (len > 64) fail("ioctl-argument-size", -1);
    struct msm_ccmd_ioctl_simple_req *req = (void *)buffer;
    req->hdr = (struct vdrm_ccmd_req){MSM_CCMD_IOCTL_SIMPLE, sizeof(*req) + len, ++*seq, 0};
    req->cmd = cmd;
    memcpy(req->payload, arg, len);
    struct msm_ccmd_ioctl_simple_rsp *rsp = response;
    rsp->hdr.len = sizeof(*rsp) + ((_IOC_DIR(cmd) & _IOC_READ) ? len : 0);
    rsp->ret = -12345;
    int ret = ctx->submit_cmd(ctx, buffer, req->hdr.len);
    if (ret || __atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE) != *seq ||
        shmem->async_error || rsp->ret) fail("ioctl-response", ret ? ret : rsp->ret);
    if (_IOC_DIR(cmd) & _IOC_READ) memcpy(arg, rsp->payload, len);
}

static void execute_gpu(struct virgl_context *ctx, struct msm_shmem *shmem,
                        void *response, uint32_t *words, int bo_fd, uint64_t iova, size_t bytes)
{
    uint32_t seq = 2;
    struct drm_msm_submitqueue queue = {0};
    simple_ioctl(ctx, shmem, response, &seq, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &queue, sizeof(queue));
    printf("QUEUE id=%u priority=%u\n", queue.id, queue.prio);
    enum { DEST_WORD = 1024, WRITE_WORDS = 16, COMMAND_WORDS = 3 + WRITE_WORDS, ROUNDS = 4 };
    uint32_t *expected = malloc(bytes);
    if (!expected) fail("expected-allocation", -1);
    cpu_sync(bo_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    memcpy(expected, words, bytes);
    cpu_sync(bo_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    uint64_t begin = now_ns();
    for (uint32_t fence = 1; fence <= ROUNDS; fence++) {
        /* Freedreno adreno_pm4.xml: CP_MEM_WRITE=0x3d, A5XX+ 64-bit
         * address followed by values. Address is strictly inside our own BO.
         * Only a CP write is exercised here; this is not shader/Metal execution. */
        cpu_sync(bo_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
        if (AdrenoPM4Type7(0x3d, 2 + WRITE_WORDS, &words[0]) != AdrenoPM4Success)
            fail("pm4-header", -1);
        uint64_t destination = iova + DEST_WORD * sizeof(uint32_t);
        words[1] = destination;
        words[2] = destination >> 32;
        for (size_t n = 0; n < WRITE_WORDS; n++) {
            words[3 + n] = 0xc0830000u ^ (fence << 8) ^ (uint32_t)n;
            words[DEST_WORD + n] = 0xdead0000u ^ (uint32_t)n;
            expected[DEST_WORD + n] = words[3 + n];
        }
        memcpy(expected, words, COMMAND_WORDS * sizeof(uint32_t));
        cpu_sync(bo_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
        unsigned char command[sizeof(struct msm_ccmd_gem_submit_req) +
                              sizeof(struct drm_msm_gem_submit_bo) +
                              sizeof(struct drm_msm_gem_submit_cmd)] = {0};
        struct msm_ccmd_gem_submit_req *req = (void *)command;
        req->hdr = (struct vdrm_ccmd_req){MSM_CCMD_GEM_SUBMIT, sizeof(command), ++seq, 0};
        req->flags = MSM_PIPE_3D0 | MSM_SUBMIT_FENCE_SN_IN | MSM_SUBMIT_NO_IMPLICIT;
        req->queue_id = queue.id;
        req->nr_bos = req->nr_cmds = 1;
        req->fence = fence;
        struct drm_msm_gem_submit_bo bo = {
            .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE, .handle = 2, .presumed = iova};
        struct drm_msm_gem_submit_cmd cmd = {
            .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 0, .size = COMMAND_WORDS * sizeof(uint32_t)};
        memcpy(req->payload, &bo, sizeof(bo));
        memcpy(req->payload + sizeof(bo), &cmd, sizeof(cmd));
        int ret = ctx->submit_cmd(ctx, command, sizeof(command));
        if (ret || shmem->async_error ||
            __atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE) != seq)
            fail("gpu-submit", ret ? ret : -EIO);
        uint64_t deadline = now_ns() + UINT64_C(5000000000);
        unsigned polls = 0;
        for (;;) {
            struct msm_ccmd_wait_fence_req wait = {
                .hdr = {MSM_CCMD_WAIT_FENCE, sizeof(wait), ++seq, 0},
                .queue_id = queue.id, .fence = fence};
            struct msm_ccmd_wait_fence_rsp *rsp = response;
            rsp->hdr.len = sizeof(*rsp);
            rsp->ret = -12345;
            ret = ctx->submit_cmd(ctx, &wait, sizeof(wait));
            if (ret || shmem->async_error ||
                __atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE) != seq)
                fail("wait-response", ret ? ret : -EIO);
            polls++;
            if (!rsp->ret) break;
            if ((rsp->ret != -ETIMEDOUT && rsp->ret != -EDEADLK) || now_ns() >= deadline)
                fail("gpu-fence", rsp->ret);
            usleep(1000);
        }
        cpu_sync(bo_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        for (size_t n = 0; n < bytes / 4; n++) {
            if (words[n] != expected[n]) {
                fprintf(stderr, "MISMATCH word=%zu got=%08x expected=%08x\n", n, words[n], expected[n]);
                fail("gpu-readback-or-canary", -1);
            }
        }
        cpu_sync(bo_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        printf("GPU fence=%u polls=%u written=%u canaries=%zu async_error=%u\n",
               fence, polls, WRITE_WORDS, bytes / 4 - WRITE_WORDS - COMMAND_WORDS, shmem->async_error);
    }
    printf("EXECUTION rounds=%u elapsed_ms=%.3f\n", ROUNDS, (now_ns() - begin) / 1e6);
    simple_ioctl(ctx, shmem, response, &seq, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &queue.id, sizeof(queue.id));
    free(expected);
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(30);
    int fd = open(KGSL_DEVICE_NODE, O_RDWR | O_CLOEXEC);
    if (fd < 0) fail("open", fd);
    struct virgl_renderer_capset_drm cap = {0};
    int ret = drm2kgsl_renderer_probe(fd, &cap);
    if (ret) fail("probe", ret);
    printf("CAPSET gpu=%u chip=0x%" PRIx64 " va=0x%" PRIx64 " size=0x%" PRIx64 " gmem=%u\n",
           cap.u.msm.gpu_id, cap.u.msm.chip_id, cap.u.msm.va_start, cap.u.msm.va_size, cap.u.msm.gmem_size);
    if (cap.u.msm.chip_id != UINT64_C(0x44050000) || !cap.u.msm.va_size)
        fail("device-mismatch", -1);
    /* probe retains its own VBO keeper descriptor; create owns this fd. */
    struct virgl_context *ctx = drm2kgsl_renderer_create(fd, 12, "openoos-test");
    if (!ctx) fail("context", -1);
    /* This arena is our own ordinary host memfd. No private VM pages are accessed. */
    const size_t arena_size = 16u << 20, bo_offset = 4u << 20, bo_size = 64u << 10;
    int arena_fd = memfd_create("openoos-gpu-qualification", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (arena_fd < 0 || ftruncate(arena_fd, arena_size) ||
        fcntl(arena_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)) fail("arena-create", -1);
    void *arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_SHARED, arena_fd, 0);
    if (arena == MAP_FAILED) fail("arena-map", -1);
    env_u64("CROSVM_DRM2KGSL_ARENA_FD", arena_fd);
    env_u64("CROSVM_DRM2KGSL_ARENA_HOST_VA", (uintptr_t)arena);
    env_u64("CROSVM_DRM2KGSL_ARENA_SIZE", arena_size);
    struct virgl_context_blob blob = {0};
    const size_t size = 16384;
    ret = ctx->get_blob(ctx, 1, 0, size, VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE, &blob);
    if (ret) fail("response-blob", ret);
    void *mapping = blob.map_ptr;
    int mapped = !mapping;
    if (mapped) mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, blob.u.fd, blob.fd_offset);
    if (mapping == MAP_FAILED) fail("mmap-response", -1);
    if (mapped || mapping != (char *)arena + blob.fd_offset || blob.fd_offset + size > bo_offset)
        fail("shared-response-arena", -1);
    struct msm_shmem *shmem = mapping;
    uint32_t offset = shmem->base.rsp_mem_offset;
    if (offset < sizeof(*shmem) || offset > size - 64) fail("response-bounds", -1);
    unsigned char command[sizeof(struct msm_ccmd_ioctl_simple_req) + sizeof(struct drm_msm_param)] = {0};
    struct msm_ccmd_ioctl_simple_req *req = (void *)command;
    req->hdr = (struct vdrm_ccmd_req){MSM_CCMD_IOCTL_SIMPLE, sizeof(command), 1, 0};
    req->cmd = DRM_IOCTL_MSM_GET_PARAM;
    struct drm_msm_param param = {.pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID};
    memcpy(req->payload, &param, sizeof(param));
    struct msm_ccmd_ioctl_simple_rsp *rsp = (void *)((char *)mapping + offset);
    rsp->hdr.len = sizeof(*rsp) + sizeof(param);
    rsp->ret = -12345;
    ret = ctx->submit_cmd(ctx, command, sizeof(command));
    if (ret) fail("submit-get-param", ret);
    uint32_t seq = __atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE);
    memcpy(&param, rsp->payload, sizeof(param));
    if (seq != 1 || rsp->ret || shmem->async_error || param.value != cap.u.msm.gpu_id)
        fail("response-validation", rsp->ret);
    printf("RESPONSE seq=%u ret=%d gpu=%" PRIu64 " async_error=%u\n", seq, rsp->ret, (uint64_t)param.value, shmem->async_error);
    /* Establish CPU backing before exporting the shared BO. */
    memset((char *)arena + bo_offset, 0, bo_size);
    struct udmabuf_create create = {.memfd = arena_fd, .flags = UDMABUF_FLAGS_CLOEXEC,
                                   .offset = bo_offset, .size = bo_size};
    int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf < 0) fail("udmabuf-open", -1);
    int bo_fd = ioctl(udmabuf, UDMABUF_CREATE, &create);
    if (bo_fd < 0) fail("udmabuf-create", -1);
    close(udmabuf);
    struct msm_ccmd_gem_new_req gem = {
        .hdr = {MSM_CCMD_GEM_NEW, sizeof(gem), 2, 0},
        .iova = cap.u.msm.va_start + (16u << 20), .size = bo_size,
        .flags = MSM_BO_GUEST_ALLOC | MSM_BO_CACHED_COHERENT, .blob_id = 1};
    ret = ctx->submit_cmd(ctx, &gem, sizeof(gem));
    if (ret || shmem->async_error) fail("gem-new", ret);
    ret = ctx->set_guest_blob_fd(ctx, 1, bo_fd); /* transfers fd ownership */
    if (ret) fail("guest-blob-fd", ret);
    struct virgl_context_blob gpu_blob = {0};
    ret = ctx->get_blob(ctx, 2, 1, bo_size, VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE, &gpu_blob);
    if (ret) fail("gpu-import-bind", ret);
    uint32_t *gpu_mapping = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 gpu_blob.u.fd, gpu_blob.fd_offset);
    if (gpu_mapping == MAP_FAILED) fail("gpu-blob-map", -1);
    uint32_t *words = (void *)((char *)arena + bo_offset);
    for (size_t i = 0; i < bo_size / 4; i++) words[i] = 0xa5830000u ^ (uint32_t)i;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (size_t i = 0; i < bo_size / 4; i++)
        if (gpu_mapping[i] != (0xa5830000u ^ (uint32_t)i)) fail("shared-alias-readback", -1);
    printf("BUFFER bytes=%zu shared_words=%zu iova=0x%" PRIx64 " imported=true bound=true async_error=%u\n",
           bo_size, bo_size / 4, gem.iova, shmem->async_error);
    execute_gpu(ctx, shmem, rsp, words, gpu_blob.u.fd, gem.iova, bo_size);
    if (munmap(gpu_mapping, bo_size)) fail("gpu-blob-unmap", -1);
    close(gpu_blob.u.fd);
    if (mapped && munmap(mapping, size)) fail("unmap-response", -1);
    close(blob.u.fd);
    ctx->destroy(ctx);
    if (munmap(arena, arena_size)) fail("arena-unmap", -1);
    close(arena_fd);
    puts("RESULT native_context=true response_protocol=true shared_bo=true gpu_execution=true guest_metal=false");
    return 0;
}
