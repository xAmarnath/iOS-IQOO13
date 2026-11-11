/* Synthetic guest rings -> production decoder -> real drm2kgsl -> Adreno.
 * This is an Android transport test, not a running XNU/Metal qualification. */
#include "vgpu-native.h"
#include "vgpu-host.h"
#include "vgpu-queue.h"
#include "vgpu-shared.h"
#include "virglrenderer.h"
#include "virgl_context.h"
#include "drm2kgsl/drm2kgsl_renderer.h"
#include "drm-uapi/msm_drm.h"
#include "msm/msm_proto.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "AdrenoPM4.h"

#define BASE VGPU_COMMAND_BASE
#define COMMANDS VGPU_COMMAND_SIZE
#define POOL VGPU_POOL_SIZE
static struct vgpu_device device;
static unsigned char *shared;
static uint16_t sequence;
static unsigned checks;
static struct vgpu_host host;
static int use_ipc;
static int memory_fd;
static uint32_t command_seq;
static void wr(uint32_t offset, uint32_t v) { assert(!(use_ipc ? vgpu_host_mmio(&host, offset, 4, 1, &v) : vgpu_mmio(&device, offset, 4, 1, &v))); }
static void wr64(uint32_t offset, uint64_t v) { wr(offset, (uint32_t)v); wr(offset + 4, (uint32_t)(v >> 32)); }
static void setup(void) {
    wr(0x70, 0); memset(shared, 0, COMMANDS); sequence = 0;
    wr(0x70, 3); wr(0x20, (uint32_t)VGPU_FEATURES); wr(0x24, 1); wr(0x20, 1); wr(0x70, 11);
    wr(0x38, 512); wr64(0x80, BASE); wr64(0x90, BASE + 8192); wr64(0xa0, BASE + 10240);
    wr(0x44, 1); wr(0x70, 15);
}
static void *submit_request(const void *input, size_t bytes, uint32_t expected) {
    assert(bytes <= 4096);
    memcpy(shared + 0x10000, input, bytes); memset(shared + 0x20000, 0xa7, 4096);
    struct vgpu_desc d[] = {{BASE + 0x10000, bytes, 1, 1}, {BASE + 0x20000, 4096, 2, 0}};
    memcpy(shared, d, sizeof d);
    uint16_t *avail = (void *)(shared + 8192);
    avail[2 + (sequence % 512)] = 0;
    __atomic_store_n(&avail[1], ++sequence, __ATOMIC_RELEASE);
    wr(0x50, 0);
    assert(__atomic_load_n((uint16_t *)(shared + 10242), __ATOMIC_ACQUIRE) == sequence);
    uint32_t used[2]; memcpy(used, shared + 10244 + ((sequence - 1) % 512) * 8, 8);
    assert(!used[0] && used[1] >= 24 && used[1] <= 4096);
    struct vgpu_header h; memcpy(&h, shared + 0x20000, sizeof h);
    if (h.type != expected) fprintf(stderr, "command=%x got=%x expected=%x\n", ((const struct vgpu_header *)input)->type, h.type, expected);
    assert(h.type == expected);
    for (unsigned i = used[1]; i < 4096; ++i) assert(shared[0x20000 + i] == 0xa7);
    checks++; return shared + 0x20000;
}
static uint64_t now_ns(void) {
    struct timespec ts; assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void cpu_sync(int fd, uint64_t flags) {
    struct dma_buf_sync s = {.flags = flags}; int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); } while (rc && errno == EINTR);
    assert(!rc);
}
static void context_ioctl(uint32_t context, struct msm_shmem *shmem, uint32_t command, void *arg, size_t len) {
    unsigned char wire[128] = {0};
    assert(52 + len <= sizeof wire);
    struct vgpu_header h = {.type = 0x207, .context = context}; memcpy(wire, &h, sizeof h);
    uint32_t prefix[] = {20 + len, 0, MSM_CCMD_IOCTL_SIMPLE, 20 + len, ++command_seq, 0, command};
    memcpy(wire + 24, prefix, sizeof prefix); memcpy(wire + 52, arg, len);
    struct msm_ccmd_ioctl_simple_rsp *rsp = (void *)((char *)shmem + shmem->base.rsp_mem_offset);
    rsp->hdr.len = 8 + ((_IOC_DIR(command) & _IOC_READ) ? len : 0); rsp->ret = -12345;
    submit_request(wire, 52 + len, VGPU_RESP_OK);
    assert(shmem->base.seqno == command_seq && !shmem->async_error && !rsp->ret);
    if (_IOC_DIR(command) & _IOC_READ) memcpy(arg, rsp->payload, len);
}
static void simple_ioctl(struct msm_shmem *shmem, uint32_t command, void *arg, size_t len) {
    context_ioctl(1, shmem, command, arg, len);
}
static void context_capacity_test(void) {
    /* Exercise real renderer VA slots and maximal response allocations together.
     * Repeat with the same IDs to prove context destruction releases both. */
    enum { COUNT = 30, RESPONSE_SIZE = 1u << 20 };
    uint32_t offsets[COUNT];
    setup();
    struct { struct vgpu_header h; uint32_t id, version; } ask = {
        .h = {.type = 0x109}, .id = 6};
    unsigned char *cap_reply = submit_request(&ask, sizeof ask, 0x1103);
    struct virgl_renderer_capset_drm cap; memcpy(&cap, cap_reply + 24, sizeof cap);
    assert(cap.u.msm.gpu_id && cap.u.msm.chip_id == UINT64_C(0x44050000));
    for (unsigned round = 0; round < 2; ++round) {
        struct { struct vgpu_header h; uint32_t length, init; char name[64]; } create = {
            .h = {.type = 0x200}, .init = 6};
        for (unsigned i = 0; i < COUNT; ++i) {
            create.h.context = 100 + i;
            int n = snprintf(create.name, sizeof create.name, "capacity-%u-%u", round, i);
            assert(n > 0 && (size_t)n < sizeof create.name);
            create.length = n;
            submit_request(&create, sizeof create, VGPU_RESP_OK);
            struct { struct vgpu_header h; uint32_t id, memory, flags, entries; uint64_t blob, size; } blob = {
                .h = {.type = 0x10c, .context = 100 + i}, .id = 100 + i,
                .memory = 2, .flags = 1, .size = RESPONSE_SIZE};
            submit_request(&blob, sizeof blob, VGPU_RESP_OK);
            struct { struct vgpu_header h; uint32_t id, pad; uint64_t offset; } map = {
                .h = {.type = 0x208, .context = 100 + i}, .id = 100 + i};
            unsigned char *reply = submit_request(&map, sizeof map, 0x1106);
            uint32_t mapping[2]; memcpy(mapping, reply + 24, sizeof mapping);
            assert(mapping[0] == (1u << 31) && !(mapping[1] & 16383));
            assert(mapping[1] <= vgpu_response_bytes(POOL) - RESPONSE_SIZE);
            offsets[i] = mapping[1];
            for (unsigned j = 0; j < i; ++j)
                assert((uint64_t)offsets[i] + RESPONSE_SIZE <= offsets[j] ||
                       (uint64_t)offsets[j] + RESPONSE_SIZE <= offsets[i]);
            uint32_t *canary = (void *)(shared + COMMANDS + offsets[i] + RESPONSE_SIZE - 4);
            *canary = 0xca830000u ^ (round << 8) ^ i;
        }
        submit_request(&create, sizeof create, VGPU_RESP_INVALID); /* duplicate at capacity */
        create.h.context = 100 + COUNT;
        submit_request(&create, sizeof create, 0x1201); /* bounded exhaustion */
        /* All clients remain usable after the failed allocation. */
        for (unsigned i = 0; i < COUNT; ++i) {
            struct msm_shmem *shmem = (void *)(shared + COMMANDS + offsets[i]);
            assert(shmem->base.rsp_mem_offset >= sizeof *shmem &&
                   shmem->base.rsp_mem_offset <= RESPONSE_SIZE - 32);
            struct drm_msm_param param = {.pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID};
            context_ioctl(100 + i, shmem, DRM_IOCTL_MSM_GET_PARAM, &param, sizeof param);
            assert(param.value == cap.u.msm.gpu_id);
            const uint32_t *canary = (void *)(shared + COMMANDS + offsets[i] + RESPONSE_SIZE - 4);
            assert(*canary == (0xca830000u ^ (round << 8) ^ i));
        }
        /* Destroy in alternating order; mapped response resources follow owners. */
        for (unsigned parity = 0; parity < 2; ++parity)
            for (unsigned i = parity; i < COUNT; i += 2) {
                struct vgpu_header destroy = {.type = 0x201, .context = 100 + i};
                submit_request(&destroy, sizeof destroy, VGPU_RESP_OK);
            }
        printf("CONTEXT_CAPACITY round=%u live=%u response_bytes=%u nonoverlap=true exhaustion_rejected=true ioctl=true released=true\n",
               round, COUNT, COUNT * RESPONSE_SIZE);
    }
}
static void execute_gpu(struct msm_shmem *shmem, uint32_t *words, uint64_t iova, unsigned cycle, unsigned round) {
    enum { BYTES = 65536, DEST = 1024, WRITES = 16, DW = 3 + WRITES };
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC); assert(dev >= 0);
    struct udmabuf_create export = {.memfd = memory_fd, .flags = UDMABUF_FLAGS_CLOEXEC,
        .offset = COMMANDS + vgpu_response_bytes(POOL), .size = BYTES};
    int dmabuf = ioctl(dev, UDMABUF_CREATE, &export); close(dev); assert(dmabuf >= 0);
    struct drm_msm_submitqueue queue = {0};
    simple_ioctl(shmem, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &queue, sizeof queue);
    printf("GPU_QUEUE cycle=%u round=%u id=%u\n", cycle, round, queue.id);
    uint32_t *expected = malloc(BYTES); assert(expected);
    cpu_sync(dmabuf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    memcpy(expected, words, BYTES);
    assert(AdrenoPM4Type7(0x3d, 2 + WRITES, &words[0]) == AdrenoPM4Success);
    words[1] = iova + DEST * 4; words[2] = (iova + DEST * 4) >> 32;
    for (unsigned i = 0; i < WRITES; ++i) {
        words[3 + i] = 0xc0830000u ^ (cycle << 16) ^ (round << 8) ^ i;
        words[DEST + i] = 0xdead0000u ^ i;
        expected[DEST + i] = words[3 + i];
    }
    memcpy(expected, words, DW * 4);
    cpu_sync(dmabuf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    unsigned char wire[32 + sizeof(struct msm_ccmd_gem_submit_req) +
        sizeof(struct drm_msm_gem_submit_bo) + sizeof(struct drm_msm_gem_submit_cmd)] = {0};
    struct vgpu_header h = {.type = 0x207, .context = 1}; memcpy(wire, &h, sizeof h);
    uint32_t native_size = sizeof wire - 32; memcpy(wire + 24, &native_size, 4);
    struct msm_ccmd_gem_submit_req *req = (void *)(wire + 32);
    req->hdr = (struct vdrm_ccmd_req){MSM_CCMD_GEM_SUBMIT, native_size, ++command_seq, 0};
    req->flags = MSM_PIPE_3D0 | MSM_SUBMIT_FENCE_SN_IN | MSM_SUBMIT_NO_IMPLICIT;
    req->queue_id = queue.id; req->nr_bos = req->nr_cmds = 1; req->fence = 1;
    struct drm_msm_gem_submit_bo bo = {.flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE, .handle = 2, .presumed = iova};
    struct drm_msm_gem_submit_cmd cmd = {.type = MSM_SUBMIT_CMD_BUF, .size = DW * 4};
    memcpy(req->payload, &bo, sizeof bo); memcpy(req->payload + sizeof bo, &cmd, sizeof cmd);
    /* Invalid work must never reach KGSL; share the next real seqno so a
     * rejection cannot silently consume it. */
    req->queue_id ^= 0x80000000u; submit_request(wire, sizeof wire, VGPU_RESP_INVALID); req->queue_id = queue.id;
    req->nr_bos = UINT32_MAX; submit_request(wire, sizeof wire, VGPU_RESP_INVALID); req->nr_bos = 1;
    req->flags |= MSM_SUBMIT_SUDO; submit_request(wire, sizeof wire, VGPU_RESP_INVALID); req->flags &= ~MSM_SUBMIT_SUDO;
    req->fence = UINT32_MAX; submit_request(wire, sizeof wire, VGPU_RESP_INVALID); req->fence = 1;
    bo.handle = 1; memcpy(req->payload, &bo, sizeof bo); submit_request(wire, sizeof wire, VGPU_RESP_INVALID); bo.handle = 2;
    bo.presumed += 16384; memcpy(req->payload, &bo, sizeof bo); submit_request(wire, sizeof wire, VGPU_RESP_INVALID); bo.presumed = iova;
    bo.flags = MSM_SUBMIT_BO_WRITE; memcpy(req->payload, &bo, sizeof bo); submit_request(wire, sizeof wire, VGPU_RESP_INVALID);
    bo.flags |= MSM_SUBMIT_BO_READ; memcpy(req->payload, &bo, sizeof bo);
    cmd.submit_idx = 1; memcpy(req->payload + sizeof bo, &cmd, sizeof cmd); submit_request(wire, sizeof wire, VGPU_RESP_INVALID); cmd.submit_idx = 0;
    cmd.submit_offset = BYTES - 4; memcpy(req->payload + sizeof bo, &cmd, sizeof cmd); submit_request(wire, sizeof wire, VGPU_RESP_INVALID); cmd.submit_offset = 0;
    cmd.nr_relocs = 1; memcpy(req->payload + sizeof bo, &cmd, sizeof cmd); submit_request(wire, sizeof wire, VGPU_RESP_INVALID); cmd.nr_relocs = 0;
    memcpy(req->payload + sizeof bo, &cmd, sizeof cmd);
    uint64_t begin = now_ns();
    submit_request(wire, sizeof wire, VGPU_RESP_OK);
    assert(shmem->base.seqno == command_seq && !shmem->async_error);
    req->hdr.seqno = command_seq + 1;
    submit_request(wire, sizeof wire, VGPU_RESP_INVALID); /* duplicate GPU timestamp */
    struct { struct vgpu_header h; uint32_t id, pad; } unref = {.h = {.type = 0x102}, .id = 2};
    submit_request(&unref, sizeof unref, VGPU_RESP_INVALID); /* unacknowledged completion retains BO */
    struct vgpu_header destroy = {.type = 0x201, .context = 1};
    submit_request(&destroy, sizeof destroy, VGPU_RESP_INVALID);
    struct { struct vgpu_header h; uint32_t size, pad; struct msm_ccmd_wait_fence_req req; } wait = {
        .h = {.type = 0x207, .context = 1}, .size = sizeof(struct msm_ccmd_wait_fence_req),
        .req = {.hdr = {MSM_CCMD_WAIT_FENCE, sizeof(struct msm_ccmd_wait_fence_req), 0, 0}, .queue_id = queue.id, .fence = 1}};
    struct msm_ccmd_wait_fence_rsp *rsp = (void *)((char *)shmem + shmem->base.rsp_mem_offset);
    wait.req.hdr.seqno = command_seq + 1; rsp->hdr.len = sizeof *rsp;
    wait.req.fence = 2; submit_request(&wait, sizeof wait, VGPU_RESP_INVALID); wait.req.fence = 1;
    wait.req.queue_id ^= 0x80000000u; submit_request(&wait, sizeof wait, VGPU_RESP_INVALID); wait.req.queue_id = queue.id;
    unsigned polls = 0;
    do {
        wait.req.hdr.seqno = ++command_seq; rsp->hdr.len = sizeof *rsp; rsp->ret = -12345;
        submit_request(&wait, sizeof wait, VGPU_RESP_OK); polls++;
        assert(shmem->base.seqno == command_seq && !shmem->async_error);
        if (!rsp->ret) break;
        assert((rsp->ret == -ETIMEDOUT || rsp->ret == -EDEADLK) && now_ns() - begin < UINT64_C(5000000000));
        usleep(1000);
    } while (1);
    cpu_sync(dmabuf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    for (unsigned i = 0; i < BYTES / 4; ++i) {
        if (words[i] != expected[i]) fprintf(stderr, "GPU_MISMATCH word=%u got=%08x expected=%08x\n", i, words[i], expected[i]);
        assert(words[i] == expected[i]);
    }
    cpu_sync(dmabuf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    printf("GPU_EXECUTION cycle=%u round=%u fence=1 polls=%u written=%u canaries=%u elapsed_ms=%.3f\n",
        cycle, round, polls, WRITES, BYTES / 4 - WRITES - DW, (now_ns() - begin) / 1e6);
    simple_ioctl(shmem, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &queue.id, sizeof queue.id);
    close(dmabuf); free(expected);
}
static void resource_pressure_test(const struct virgl_renderer_capset_drm *cap, struct msm_shmem *shmem, unsigned cycle) {
    enum { COUNT=96, SIZE=65536 };
    struct { struct vgpu_header h; uint32_t bytes, pad; struct msm_ccmd_gem_new_req gem; } request = {
        .h={.type=0x207,.context=1},.bytes=sizeof(struct msm_ccmd_gem_new_req),
        .gem={.hdr={MSM_CCMD_GEM_NEW,sizeof(struct msm_ccmd_gem_new_req),0,0},
              .size=SIZE,.flags=MSM_BO_GUEST_ALLOC|MSM_BO_CACHED_COHERENT}};
    struct { struct vgpu_header h; uint32_t id,memory,flags,entries; uint64_t blob,size,address; uint32_t length,pad; } create={
        .h={.type=0x10c,.context=1},.memory=3,.flags=1,.entries=1,.size=SIZE,.length=SIZE};
    struct { struct vgpu_header h; uint32_t id,pad; uint64_t offset; } map={.h={.type=0x208,.context=1}};
    for (unsigned i=0;i<COUNT;i++) {
        unsigned offset=vgpu_response_bytes(POOL)+i*SIZE;
        request.gem.hdr.seqno=++command_seq; request.gem.blob_id=i+1;
        request.gem.iova=cap->u.msm.va_start+(16u<<20)+i*SIZE;
        submit_request(&request,sizeof request,VGPU_RESP_OK);
        create.id=i+2; create.blob=i+1; create.address=VGPU_POOL_BASE+offset;
        uint32_t *words=(void *)(shared+COMMANDS+offset);
        words[0]=0xa5830000u^(cycle<<20)^i; words[SIZE/4-1]=~words[0];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        submit_request(&create,sizeof create,VGPU_RESP_OK);
        map.id=create.id;
        unsigned char *reply=submit_request(&map,sizeof map,0x1106);
        uint32_t mapping[2]; memcpy(mapping,reply+24,sizeof mapping);
        assert(mapping[0]==(1u<<31) && mapping[1]==offset);
    }
    assert(shmem->base.seqno==command_seq && !shmem->async_error);
    for (unsigned i=COUNT;i-- > 0;) {
        uint32_t *words=(void *)(shared+COMMANDS+vgpu_response_bytes(POOL)+i*SIZE);
        assert(words[0]==(0xa5830000u^(cycle<<20)^i) && words[SIZE/4-1]==~words[0]);
        struct { struct vgpu_header h; uint32_t id,pad; } unref={.h={.type=0x102},.id=i+2};
        submit_request(&unref,sizeof unref,VGPU_RESP_OK);
    }
    printf("RESOURCE_PRESSURE cycle=%u live_resources=%u bytes=%u imported=true mapped=true canaries=true released=true\n",cycle,COUNT+1,COUNT*SIZE);
}
static void shared_bo_test(const struct virgl_renderer_capset_drm *cap, struct msm_shmem *shmem, unsigned cycle) {
    const uint32_t bo_size = 65536;
    /* Exercise real import, GPU writes and fences at both ends of the pool. */
    const uint32_t bo_offset = cycle ? POOL - bo_size : vgpu_response_bytes(POOL);
    struct { struct vgpu_header h; uint32_t bytes, pad; struct msm_ccmd_gem_new_req gem; } request = {
        .h = {.type = 0x207, .context = 1}, .bytes = sizeof(struct msm_ccmd_gem_new_req),
        .gem = {.hdr = {MSM_CCMD_GEM_NEW, sizeof(struct msm_ccmd_gem_new_req), 2, 0},
            .iova = cap->u.msm.va_start + (16u << 20), .size = bo_size,
            .flags = MSM_BO_GUEST_ALLOC | MSM_BO_CACHED_COHERENT, .blob_id = 1}};
    struct { struct vgpu_header h; uint32_t id, memory, flags, entries; uint64_t blob, size;
             uint64_t address; uint32_t length, pad; } create = {
        .h = {.type = 0x10c, .context = 1}, .id = 2, .memory = 3, .flags = 1, .entries = 1,
        .blob = 1, .size = bo_size, .address = VGPU_POOL_BASE + bo_offset, .length = bo_size};
    struct { struct vgpu_header h; uint32_t id, pad; uint64_t offset; } map = {
        .h = {.type = 0x208, .context = 1}, .id = 2};
    struct { struct vgpu_header h; uint32_t id, pad; } unref = {.h = {.type = 0x102}, .id = 2};
    for (unsigned round = 0; round < 2; ++round) {
        /* Same blob/resource/IOVA reused after detach: native identity must go. */
        request.gem.hdr.seqno = ++command_seq;
        request.gem.flags = MSM_BO_CACHED_COHERENT;
        submit_request(&request, sizeof request, VGPU_RESP_INVALID);
        request.gem.flags |= MSM_BO_GUEST_ALLOC;
        submit_request(&request, sizeof request, VGPU_RESP_OK);
        submit_request(&request, sizeof request, VGPU_RESP_INVALID); /* still pending */
        uint64_t rejected[] = {UINT64_C(0x80000000), VGPU_COMMAND_BASE, VGPU_POOL_BASE,
            VGPU_POOL_BASE + vgpu_response_bytes(POOL) - 16384,
            VGPU_POOL_BASE + POOL - 16384, VGPU_POOL_BASE + POOL, UINT64_MAX - 16383,
            VGPU_POOL_BASE + bo_offset + 1};
        for (unsigned i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
            create.address = rejected[i];
            submit_request(&create, sizeof create, VGPU_RESP_INVALID);
        }
        create.address = VGPU_POOL_BASE + bo_offset;
        create.blob = 2; submit_request(&create, sizeof create, VGPU_RESP_INVALID); create.blob = 1;
        create.length -= 16384; submit_request(&create, sizeof create, VGPU_RESP_INVALID); create.length = bo_size;
        uint32_t *words = (void *)(shared + COMMANDS + bo_offset);
        for (unsigned i = 0; i < bo_size / 4; ++i) words[i] = 0xa5830000u ^ (cycle << 20) ^ i;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        submit_request(&create, sizeof create, VGPU_RESP_OK);
        submit_request(&create, sizeof create, VGPU_RESP_INVALID); /* exported already */
        unsigned char *reply = submit_request(&map, sizeof map, 0x1106);
        uint32_t mapping[2]; memcpy(mapping, reply + 24, sizeof mapping);
        assert(mapping[0] == (1u << 31) && mapping[1] == bo_offset);
        for (unsigned i = 0; i < bo_size / 4; ++i) assert(words[i] == (0xa5830000u ^ (cycle << 20) ^ i));
        assert(__atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE) == command_seq && !shmem->async_error);
        /* Overlapping IOVAs and duplicate live blob IDs are rejected before native parsing. */
        request.gem.hdr.seqno = command_seq + 1;
        request.gem.blob_id = 2;
        submit_request(&request, sizeof request, VGPU_RESP_INVALID);
        request.gem.blob_id = 1;
        submit_request(&request, sizeof request, VGPU_RESP_INVALID);
        printf("SHARED_BO cycle=%u round=%u bytes=%u pool=0x%x imported=true bound=true canaries=%u\n",
               cycle, round, bo_size, bo_offset, bo_size / 4);
        execute_gpu(shmem, words, request.gem.iova, cycle, round);
        if (cycle == 2 && round == 1) return; /* reset/destroy releases a live GPU object */
        submit_request(&unref, sizeof unref, VGPU_RESP_OK);
        submit_request(&unref, sizeof unref, 0x1203);
    }
}
int main(void) {
    setbuf(stdout, NULL); alarm(30);
    const char *backend = getenv("VGPU_BACKEND_PATH");
    use_ipc = backend != NULL;
    int fd = -1;
    struct vgpu_native *native = NULL;
    if (use_ipc) {
        int rc = vgpu_host_start(&host, backend, 1);
        if (rc) { perror("backend-start"); vgpu_host_release(&host); return 1; }
        shared = host.shared;
    } else {
        fd = memfd_create("openoos-transport-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        assert(fd >= 0 && !ftruncate(fd, COMMANDS + POOL));
        assert(!fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW));
        shared = mmap(NULL, COMMANDS + POOL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(shared != MAP_FAILED);
        native = vgpu_native_create(fd, shared + COMMANDS, POOL, COMMANDS);
        if (!native) perror("native-create");
        assert(native);
        assert(!vgpu_init(&device, shared, BASE, COMMANDS, vgpu_native_dispatch, vgpu_native_reset, native));
    }
    memory_fd = use_ipc ? host.memory : fd;
    context_capacity_test();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        command_seq = 1;
        setup();
        struct { struct vgpu_header h; uint32_t a, b; } ask = {{.type = 0x108}, 0, 0};
        unsigned char *reply = submit_request(&ask, sizeof ask, 0x1102);
        uint32_t info[4]; memcpy(info, reply + 24, sizeof info);
        assert(info[0] == 6 && info[2] == sizeof(struct virgl_renderer_capset_drm));
        ask.h.type = 0x109; ask.a = 6;
        reply = submit_request(&ask, sizeof ask, 0x1103);
        struct virgl_renderer_capset_drm cap; memcpy(&cap, reply + 24, sizeof cap);
        assert(cap.u.msm.chip_id == UINT64_C(0x44050000) && cap.u.msm.va_size);
        printf("CAPSET cycle=%u gpu=%u chip=0x%" PRIx64 " va_size=0x%" PRIx64 "\n",
               cycle, cap.u.msm.gpu_id, cap.u.msm.chip_id, cap.u.msm.va_size);
        struct { struct vgpu_header h; uint32_t length, init; char name[64]; } create = {
            {.type = 0x200, .context = 1}, 14, 6, "transport-test"};
        submit_request(&create, sizeof create, VGPU_RESP_OK);
        submit_request(&create, sizeof create, VGPU_RESP_INVALID); /* duplicate */
        struct { struct vgpu_header h; uint32_t id, memory, flags, entries; uint64_t blob, size; } blob = {
            {.type = 0x10c, .context = 1}, 1, 3, 1, 0, 0, 16384};
        submit_request(&blob, sizeof blob, VGPU_RESP_INVALID); /* cannot import private guest memory */
        blob.memory = 2; submit_request(&blob, sizeof blob, VGPU_RESP_OK);
        submit_request(&blob, sizeof blob, VGPU_RESP_INVALID);
        struct { struct vgpu_header h; uint32_t id, pad; uint64_t offset; } map = {
            {.type = 0x208, .context = 1}, 1, 0, 0};
        reply = submit_request(&map, sizeof map, 0x1106);
        uint32_t mapping[2]; memcpy(mapping, reply + 24, 8);
        assert(mapping[0] == (1u << 31) && mapping[1] <= POOL - 16384 && !(mapping[1] & 16383));
        struct msm_shmem *shmem = (void *)(shared + COMMANDS + mapping[1]);
        assert(shmem->base.rsp_mem_offset >= sizeof *shmem && shmem->base.rsp_mem_offset <= 16384 - 32);
        struct msm_ccmd_ioctl_simple_rsp *response = (void *)((char *)shmem + shmem->base.rsp_mem_offset);
        response->hdr.len = 32; response->ret = -12345;
        /* Packed native ccmd's ioctl payload begins at byte20, not C's natural
         * alignment for drm_msm_param. Assemble exactly as XNU selftest does. */
        unsigned char submit[32 + 44] = {0};
        struct vgpu_header h = {.type = 0x207, .context = 1}; memcpy(submit, &h, 24);
        uint32_t prefix[] = {44, 0, MSM_CCMD_IOCTL_SIMPLE, 44, 1, 0, DRM_IOCTL_MSM_GET_PARAM};
        memcpy(submit + 24, prefix, sizeof prefix);
        struct drm_msm_param param = {.pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID};
        memcpy(submit + 52, &param, sizeof param);
        submit_request(submit, sizeof submit, VGPU_RESP_OK);
        memcpy(&param, response->payload, sizeof param);
        assert(__atomic_load_n(&shmem->base.seqno, __ATOMIC_ACQUIRE) == 1);
        assert(!response->ret && !shmem->async_error && param.value == cap.u.msm.gpu_id);
        printf("RESPONSE cycle=%u pool_offset=0x%x seq=1 ret=%d gpu=%" PRIu64 " async_error=%u\n",
               cycle, mapping[1], response->ret, (uint64_t)param.value, shmem->async_error);
        uint32_t bad = UINT32_MAX; memcpy(submit + 44, &bad, 4);
        submit_request(submit, sizeof submit, VGPU_RESP_INVALID); /* native response bounds */
        bad = 0; memcpy(submit + 44, &bad, 4);
        bad = MSM_CCMD_GEM_NEW; memcpy(submit + 32, &bad, 4);
        submit_request(submit, sizeof submit, VGPU_RESP_INVALID); /* wrong GEM_NEW layout */
        assert(shmem->base.seqno == 1 && !shmem->async_error);
        resource_pressure_test(&cap, shmem, cycle);
        shared_bo_test(&cap, shmem, cycle);
        struct { struct vgpu_header h; uint32_t id, pad; } resource = {{.type = 0x209, .context = 1}, 1, 0};
        if (cycle == 2) break; /* reset must release a still-mapped live context */
        submit_request(&resource, sizeof resource, VGPU_RESP_OK);
        resource.h.type = 0x102; resource.h.context = 0; submit_request(&resource, sizeof resource, VGPU_RESP_OK);
        submit_request(&resource, sizeof resource, 0x1203);
        h.type = 0x201; submit_request(&h, sizeof h, VGPU_RESP_OK);
        submit_request(&h, sizeof h, 0x1204);
    }
    if (use_ipc) {
        assert(!vgpu_shared_verify(host.shared, VGPU_MEMORY_SIZE));
        assert(!vgpu_host_release(&host));
    } else {
        assert(!device.errors);
        vgpu_destroy(&device); assert(!vgpu_native_destroy(native));
        assert(!munmap(shared, COMMANDS + POOL)); close(fd);
    }
    printf("IPC backend_process=%d\n", use_ipc);
    printf("RESULT native_transport=true real_gpu_id=true shared_bo=true gpu_execution=true native_fences=true cycles=3 checks=%u malformed_rejected=true context_capacity=30 context_reuse=true guest_metal=false\n", checks);
}
