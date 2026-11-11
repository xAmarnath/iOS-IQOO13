/* Actual drm2kgsl adapter. Unsupported work returns an error; never fabricate
 * a GPU response or report a fence complete without a retirement mechanism. */
#include "vgpu-native.h"
#include "vgpu-queue.h"
#include "vgpu-shared.h"
#include "virgl_context.h"
#include "virgl_resource.h"
#include "virglrenderer.h"
#include "drm2kgsl/drm2kgsl_renderer.h"
#include "drm_context.h"
#include "drm-uapi/msm_drm.h"
#include "msm/msm_proto.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Global table shared by all clients. SpringBoard + backboardd exhaust the
 * probe-sized 64 slots with only 94 MiB resident. Keep metadata and submit
 * scratch bounded (192 KiB / 48 KiB), independently of the BO arena. */
/* The guest permits 64 clients and the qualified native VA layout has 32
 * slices. The old 16-slot adapter cap rejected clients with BO space left.
 * Fit 30 maximal response allocations plus the existing guard in 32 MiB;
 * do not shrink a client's VA slice or enlarge the shared memory mapping. */
enum { CONTEXTS = 30, RESOURCES = 4096, QUEUES = 8, ERR_UNSPEC = 0x1200,
       ERR_MEMORY = 0x1201, ERR_RESOURCE = 0x1203, ERR_CONTEXT = 0x1204 };
_Static_assert(VGPU_RESPONSE_BYTES >= (CONTEXTS + 2u) * (1u << 20),
               "Response arena must fit every maximal context and the base guard");
struct native_queue {
    uint32_t id, submitted, completed;
    int active, uncertain;
};
struct native_context {
    uint32_t id;
    char name[65];
    struct virgl_context *ctx;
    int has_shmem, failed;
    /* One staged GEM_NEW until CREATE_BLOB pairs it with shared backing. */
    struct msm_ccmd_gem_new_req pending;
    uint32_t seqno;
    struct native_queue queues[QUEUES];
};
struct native_resource {
    uint32_t id, owner, offset, size;
    int fd, mapped;
    uint32_t blob, response_offset;
    uint64_t iova;
};
struct vgpu_native {
    unsigned char *pool;
    uint64_t size, fd_offset;
    int fd;
    struct virgl_renderer_capset_drm cap;
    struct native_context contexts[CONTEXTS];
    struct native_resource resources[RESOURCES];
    uint64_t rejections;
    uint64_t created, destroyed, capacity_rejections;
    unsigned live, peak;
};
static int arena_claimed;
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int env_number(const char *name, uint64_t value) {
    char s[32]; snprintf(s, sizeof s, "0x%" PRIx64, value); return setenv(name, s, 1);
}
static struct native_context *context(struct vgpu_native *n, uint32_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < CONTEXTS; ++i) if (n->contexts[i].id == id) return &n->contexts[i];
    return NULL;
}
static struct native_resource *resource(struct vgpu_native *n, uint32_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < RESOURCES; ++i) if (n->resources[i].id == id) return &n->resources[i];
    return NULL;
}
static struct native_queue *submit_queue(struct native_context *c, uint32_t id) {
    for (unsigned i = 0; i < QUEUES; ++i)
        if (c->queues[i].active && c->queues[i].id == id) return &c->queues[i];
    return NULL;
}
static int context_busy(const struct native_context *c) {
    for (unsigned i = 0; i < QUEUES; ++i)
        if (c->queues[i].uncertain || c->queues[i].submitted != c->queues[i].completed) return 1;
    return 0;
}
static int log_count(uint64_t count) {
    return count <= 8 || !(count & (count - 1));
}
static void log_contexts(struct vgpu_native *n) {
    /* Only on exponentially sampled capacity failures. No per-frame scan. */
    for (unsigned i = 0; i < CONTEXTS; ++i) {
        struct native_context *c = &n->contexts[i];
        if (c->id) fprintf(stderr, "GPU_NATIVE_CONTEXT id=%u name=%s busy=%d failed=%d seqno=%u\n",
                           c->id, c->name, context_busy(c), c->failed, c->seqno);
    }
}
static int execute_native(struct native_context *c, const void *p, size_t len) {
    struct drm_context *dctx = to_drm_context(c->ctx);
    int rc = c->ctx->submit_cmd(c->ctx, p, len);
    c->seqno = u32((const char *)p + 8);
    if (rc || dctx->native_async_error || dctx->completed_seqno != c->seqno) {
        c->failed = 1;
        return -EIO;
    }
    return 0;
}
static void release_resource(struct vgpu_native *n, struct native_resource *r) {
    if (r->id) {
        struct native_context *c = context(n, r->owner);
        if (c && r->blob) {
            struct virgl_resource native = {.res_id = r->id};
            c->ctx->detach_resource(c->ctx, &native);
        }
        close(r->fd);
    }
    memset(r, 0, sizeof *r);
}
static void release_context(struct vgpu_native *n, struct native_context *c) {
    /* Reset cannot revoke GPU accesses. Retain uncertain/in-flight backing
     * and its IDs instead of making the same SHARE extent available again. */
    if (context_busy(c)) { c->failed = 1; return; }
    for (unsigned i = 0; i < RESOURCES; ++i)
        if (n->resources[i].id && n->resources[i].owner == c->id) release_resource(n, &n->resources[i]);
    c->ctx->destroy(c->ctx); /* owns response allocation; unref alone cannot free it */
    if (n->live) --n->live;
    if (n->destroyed != UINT64_MAX) ++n->destroyed;
    if (log_count(n->destroyed))
        fprintf(stderr, "GPU_NATIVE_CONTEXT_RELEASE id=%u name=%s live=%u peak=%u created=%" PRIu64 " destroyed=%" PRIu64 "\n",
                c->id, c->name, n->live, n->peak, n->created, n->destroyed);
    memset(c, 0, sizeof *c);
}
void vgpu_native_reset(void *opaque) {
    struct vgpu_native *n = opaque;
    if (!n) return;
    for (unsigned i = 0; i < CONTEXTS; ++i) if (n->contexts[i].id) release_context(n, &n->contexts[i]);
}
int vgpu_native_destroy(struct vgpu_native *n) {
    if (n) {
        vgpu_native_reset(n);
        for (unsigned i = 0; i < CONTEXTS; ++i) if (n->contexts[i].id) return -EBUSY;
        close(n->fd); free(n);
    }
    return 0;
}

struct vgpu_native *vgpu_native_create(int fd, void *pool, size_t size, uint64_t offset) {
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (arena_claimed || !pool || size < (8u << 20) || size > VGPU_POOL_SIZE || (size & 32767u) ||
        ((size | offset) & 16383u) || (uintptr_t)pool > UINTPTR_MAX - size || offset > UINT64_MAX - size ||
        fstat(fd, &st) || st.st_size < 0 || offset + size > (uint64_t)st.st_size ||
        seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW)) {
        errno = EINVAL; return NULL;
    }
    struct vgpu_native *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->pool = pool; n->size = size; n->fd_offset = offset;
    n->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (n->fd < 0) { free(n); return NULL; }
    if (env_number("CROSVM_DRM2KGSL_ARENA_FD", fd) ||
        env_number("CROSVM_DRM2KGSL_ARENA_HOST_VA", (uintptr_t)pool) ||
        env_number("CROSVM_DRM2KGSL_ARENA_SIZE", vgpu_response_bytes(size)) ||
        env_number("CROSVM_DRM2KGSL_ARENA_FD_OFFSET", offset)) { close(n->fd); free(n); return NULL; }
    arena_claimed = 1; /* upstream arena/probe state is process-global */
    int kgsl = open(KGSL_DEVICE_NODE, O_RDWR | O_CLOEXEC);
    int result = kgsl < 0 ? -errno : drm2kgsl_renderer_probe(kgsl, &n->cap);
    if (kgsl >= 0) close(kgsl);
    if (result) { close(n->fd); free(n); errno = result < 0 ? -result : EIO; return NULL; }
    return n;
}

static size_t answer(void *out, size_t capacity, uint32_t type, uint32_t ctx, const void *data, size_t size) {
    if (capacity < sizeof(struct vgpu_header) || size > capacity - sizeof(struct vgpu_header)) return 0;
    struct vgpu_header h = {.type = type, .context = ctx};
    memcpy(out, &h, sizeof h);
    if (size) memcpy((char *)out + sizeof h, data, size);
    return sizeof h + size;
}

size_t vgpu_native_dispatch(void *opaque, unsigned queue, const void *input, size_t bytes, void *out, size_t capacity) {
    struct vgpu_native *n = opaque;
    struct vgpu_header h;
    if (!n || bytes < sizeof h || capacity < sizeof h) return 0;
    memcpy(&h, input, sizeof h);
    const unsigned char *p = (const unsigned char *)input + sizeof h;
    size_t len = bytes - sizeof h;
    uint32_t error = VGPU_RESP_INVALID;
    if (queue || h.flags || h.ring || h.fence || h.padding[0] || h.padding[1] || h.padding[2]) goto fail;
    struct native_context *c = context(n, h.context);
    struct native_resource *r;
    switch (h.type) {
    case 0x108: { /* GET_CAPSET_INFO */
        if (len != 8 || u32(p) || u32(p + 4)) break;
        uint32_t info[] = {6, 0, sizeof n->cap, 0};
        return answer(out, capacity, 0x1102, h.context, info, sizeof info);
    }
    case 0x109: /* GET_CAPSET */
        if (len != 8 || u32(p) != 6 || u32(p + 4)) break;
        return answer(out, capacity, 0x1103, h.context, &n->cap, sizeof n->cap);
    case 0x200: { /* CTX_CREATE */
        if (len != 72 || !h.context || c || u32(p) > 64 || u32(p + 4) != 6) break;
        for (unsigned i = 0; i < CONTEXTS; ++i) if (!n->contexts[i].id) { c = &n->contexts[i]; break; }
        if (!c) {
            if (n->capacity_rejections != UINT64_MAX) ++n->capacity_rejections;
            if (log_count(n->capacity_rejections)) log_contexts(n);
            error = ERR_MEMORY; break;
        }
        int fd = open(KGSL_DEVICE_NODE, O_RDWR | O_CLOEXEC);
        if (fd < 0) { error = ERR_UNSPEC; break; }
        c->ctx = drm2kgsl_renderer_create(fd, u32(p), (const char *)p + 8); /* takes fd */
        if (!c->ctx) { error = ERR_MEMORY; break; }
        c->id = h.context; c->ctx->ctx_id = h.context;
        /* Names arrive from the guest: bounded and printable in diagnostics. */
        for (unsigned i = 0; i < u32(p); ++i)
            c->name[i] = p[8+i] >= 32 && p[8+i] <= 126 ? p[8+i] : '?';
        c->name[u32(p)] = 0;
        ++n->live;
        if (n->live > n->peak) n->peak = n->live;
        if (n->created != UINT64_MAX) ++n->created;
        if (log_count(n->created))
            fprintf(stderr, "GPU_NATIVE_CONTEXT_CREATE id=%u name=%s live=%u peak=%u created=%" PRIu64 " destroyed=%" PRIu64 "\n",
                    c->id, c->name, n->live, n->peak, n->created, n->destroyed);
        return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
    }
    case 0x201: /* CTX_DESTROY */
        if (len) break;
        if (!c) { error = ERR_CONTEXT; break; }
        if (context_busy(c)) break;
        release_context(n, c);
        return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
    case 0x10c: { /* RESOURCE_CREATE_BLOB */
        if (len >= 32 && u32(p + 4) == 3) {
            uint32_t offset = 0;
            if (!c) { error = ERR_CONTEXT; break; }
            if (c->failed || len != 48 || !u32(p) || resource(n, u32(p)) ||
                u32(p + 8) != 1 || u32(p + 12) != 1 || !u64(p + 16) ||
                u64(p + 16) != c->pending.blob_id || u64(p + 24) != c->pending.size ||
                u32(p + 40) != u64(p + 24) || u32(p + 44) ||
                !vgpu_bo_range(n->size, u64(p + 32), u64(p + 24), &offset)) break;
            r = NULL;
            int overlap = 0;
            for (unsigned i = 0; i < RESOURCES; ++i) {
                struct native_resource *other = &n->resources[i];
                if (!other->id) { if (!r) r = other; continue; }
                if (vgpu_ranges_overlap(offset, u64(p + 24), other->offset, other->size)) overlap = 1;
            }
            if (overlap) break;
            if (!r) { error = ERR_MEMORY; break; }
            struct udmabuf_create create = {.memfd = n->fd, .flags = UDMABUF_FLAGS_CLOEXEC,
                .offset = n->fd_offset + offset, .size = u64(p + 24)};
            int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            if (device < 0) { error = ERR_UNSPEC; break; }
            int backing = ioctl(device, UDMABUF_CREATE, &create);
            close(device);
            if (backing < 0) { error = ERR_MEMORY; break; }
            int rc = c->ctx->set_guest_blob_fd(c->ctx, c->pending.blob_id, backing);
            if (rc) { close(backing); error = ERR_UNSPEC; break; }
            /* The context now owns backing, including on import failure. Do
             * not retry an uncertain native bind; CTX_DESTROY releases it. */
            struct virgl_context_blob blob = {.u.fd = -1};
            rc = c->ctx->get_blob(c->ctx, u32(p), c->pending.blob_id, c->pending.size, 1, &blob);
            if (rc || blob.type != VIRGL_RESOURCE_FD_DMABUF || blob.u.fd < 0 || blob.fd_offset) {
                if (blob.u.fd >= 0) close(blob.u.fd);
                c->failed = 1; error = ERR_UNSPEC; break;
            }
            *r = (struct native_resource){.id = u32(p), .owner = h.context, .offset = offset,
                .size = c->pending.size, .fd = blob.u.fd, .blob = c->pending.blob_id, .iova = c->pending.iova};
            memset(&c->pending, 0, sizeof c->pending);
            return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
        }
        if (len != 32 || !u32(p) || u32(p + 4) != 2 || u32(p + 8) != 1 ||
            u32(p + 12) || u64(p + 16) || u64(p + 24) < 16384 ||
            u64(p + 24) > (1u << 20) || (u64(p + 24) & 16383u) || resource(n, u32(p))) break;
        if (!c) { error = ERR_CONTEXT; break; }
        if (c->has_shmem) break;
        r = NULL;
        for (unsigned i = 0; i < RESOURCES; ++i) if (!n->resources[i].id) { r = &n->resources[i]; break; }
        if (!r) { error = ERR_MEMORY; break; }
        struct virgl_context_blob blob = {.u.fd = -1};
        if (c->ctx->get_blob(c->ctx, u32(p), 0, u64(p + 24), 1, &blob)) { error = ERR_MEMORY; break; }
        c->has_shmem = 1;
        uint64_t off = blob.fd_offset - n->fd_offset;
        if (blob.type != VIRGL_RESOURCE_FD_SHM || blob.u.fd < 0 || blob.fd_offset < n->fd_offset ||
            off > vgpu_response_bytes(n->size) || u64(p + 24) > vgpu_response_bytes(n->size) - off || (off & 16383u) ||
            blob.map_ptr != n->pool + off) {
            if (blob.u.fd >= 0) close(blob.u.fd);
            error = ERR_UNSPEC; break;
        }
        *r = (struct native_resource){.id = u32(p), .owner = h.context, .offset = off,
            .size = u64(p + 24), .fd = blob.u.fd,
            .response_offset = ((struct msm_shmem *)(n->pool + off))->base.rsp_mem_offset};
        if (r->response_offset < sizeof(struct msm_shmem) || r->response_offset > r->size - 32) {
            c->failed = 1; error = ERR_UNSPEC; break;
        }
        return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
    }
    case 0x208: /* RESOURCE_MAP_BLOB */
    case 0x209: /* RESOURCE_UNMAP_BLOB */
    case 0x102: { /* RESOURCE_UNREF may use context zero */
        if (len != (h.type == 0x208 ? 16u : 8u) || u32(p + 4) || (h.type == 0x208 && u64(p + 8))) break;
        r = resource(n, u32(p));
        if (!r || (h.context && h.context != r->owner)) { error = ERR_RESOURCE; break; }
        if (h.type == 0x102 && context_busy(context(n, r->owner))) break;
        if (h.type == 0x208) {
            uint32_t info[] = {UINT32_C(1) << 31, r->offset};
            if (capacity < sizeof h + sizeof info) return 0;
            r->mapped = 1;
            return answer(out, capacity, 0x1106, h.context, info, sizeof info);
        }
        if (h.type == 0x209) r->mapped = 0;
        else release_resource(n, r);
        return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
    }
    case 0x207: { /* SUBMIT_3D: explicitly bounded native commands */
        if (len < 8 || u32(p) != len - 8 || u32(p + 4)) break;
        if (!c) { error = ERR_CONTEXT; break; }
        if (!c->has_shmem || c->failed) break;
        p += 8; len -= 8;
        if (len < sizeof(struct vdrm_ccmd_req) || u32(p + 4) != len ||
            u32(p + 8) <= c->seqno || (u32(p + 12) & 3)) break;
        r = NULL;
        for (unsigned i = 0; i < RESOURCES; ++i)
            if (n->resources[i].id && n->resources[i].owner == h.context && !n->resources[i].blob) {
                r = &n->resources[i]; break;
            }
        if (!r || !r->mapped || u32(p + 12) > r->size - r->response_offset - 32) break;
        unsigned char *response = n->pool + r->offset + r->response_offset + u32(p + 12);
        if (u32(p) == MSM_CCMD_GEM_NEW) {
            struct msm_ccmd_gem_new_req gem;
            if (len != sizeof gem || c->pending.blob_id) break;
            memcpy(&gem, p, sizeof gem);
            uint64_t start = n->cap.u.msm.va_start, size = n->cap.u.msm.va_size;
            if (!gem.blob_id || !gem.size || gem.size > n->size - vgpu_response_bytes(n->size) ||
                ((gem.size | gem.iova) & 16383u) ||
                gem.flags != (MSM_BO_GUEST_ALLOC | MSM_BO_CACHED_COHERENT) ||
                gem.iova < start || gem.iova - start > size || gem.size > size - (gem.iova - start)) break;
            int conflict = 0, available = 0;
            for (unsigned i = 0; i < RESOURCES; ++i) {
                struct native_resource *other = &n->resources[i];
                if (!other->id) { available = 1; continue; }
                if (other->owner == h.context && other->blob &&
                    (other->blob == gem.blob_id || vgpu_ranges_overlap(gem.iova, gem.size, other->iova, other->size))) conflict = 1;
            }
            if (conflict) break;
            if (!available) { error = ERR_MEMORY; break; }
            if (execute_native(c, p, len)) { error = ERR_UNSPEC; break; }
            c->pending = gem;
            return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
        }
        if (u32(p) == MSM_CCMD_GEM_SUBMIT) {
            struct msm_ccmd_gem_submit_req req;
            if (len < sizeof req || c->pending.blob_id) break;
            memcpy(&req, p, sizeof req);
            struct native_queue *q = submit_queue(c, req.queue_id);
            if (!q || q->uncertain || req.fence <= q->submitted || req.fence > INT32_MAX - 1u ||
                req.flags != (MSM_PIPE_3D0 | MSM_SUBMIT_FENCE_SN_IN | MSM_SUBMIT_NO_IMPLICIT) ||
                !req.nr_bos || req.nr_bos > RESOURCES || !req.nr_cmds || req.nr_cmds > RESOURCES ||
                len != sizeof req + req.nr_bos * sizeof(struct drm_msm_gem_submit_bo) +
                       req.nr_cmds * sizeof(struct drm_msm_gem_submit_cmd)) break;
            struct native_resource *bos[RESOURCES];
            uint32_t bo_flags[RESOURCES];
            int valid = 1;
            for (unsigned i = 0; i < req.nr_bos; ++i) {
                struct drm_msm_gem_submit_bo bo;
                memcpy(&bo, p + sizeof req + i * sizeof bo, sizeof bo);
                bos[i] = resource(n, bo.handle); bo_flags[i] = bo.flags;
                if (!bos[i] || !bos[i]->blob || bos[i]->owner != c->id ||
                    bo.presumed != bos[i]->iova ||
                    !(bo.flags & (MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE)) ||
                    (bo.flags & ~(MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE | MSM_SUBMIT_BO_NO_IMPLICIT))) valid = 0;
                for (unsigned j = 0; j < i; ++j) if (bos[j] == bos[i]) valid = 0;
            }
            if (!valid) break;
            for (unsigned i = 0; i < req.nr_cmds; ++i) {
                struct drm_msm_gem_submit_cmd cmd;
                memcpy(&cmd, p + sizeof req + req.nr_bos * sizeof(struct drm_msm_gem_submit_bo) + i * sizeof cmd, sizeof cmd);
                if (cmd.type != MSM_SUBMIT_CMD_BUF || cmd.submit_idx >= req.nr_bos ||
                    cmd.pad || cmd.nr_relocs || cmd.relocs || !cmd.size || ((cmd.size | cmd.submit_offset) & 3)) {
                    valid = 0; break;
                }
                struct native_resource *bo = bos[cmd.submit_idx];
                if (!(bo_flags[cmd.submit_idx] & MSM_SUBMIT_BO_READ) || cmd.submit_offset > bo->size ||
                    cmd.size > bo->size - cmd.submit_offset) { valid = 0; break; }
            }
            if (!valid) break;
            q->submitted = req.fence;
            if (execute_native(c, p, len)) {
                q->uncertain = 1; error = ERR_UNSPEC; break;
            }
            return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
        }
        if (u32(p) == MSM_CCMD_WAIT_FENCE) {
            struct msm_ccmd_wait_fence_req req;
            if (len != sizeof req || u32(response) != sizeof(struct msm_ccmd_wait_fence_rsp)) break;
            memcpy(&req, p, sizeof req);
            struct native_queue *q = submit_queue(c, req.queue_id);
            if (!q || !req.fence || req.fence > q->submitted || q->uncertain) break;
            if (execute_native(c, p, len)) { q->uncertain = 1; error = ERR_UNSPEC; break; }
            struct drm_context *dctx = to_drm_context(c->ctx);
            if (dctx->completed_rsp_len != sizeof(struct msm_ccmd_wait_fence_rsp)) {
                q->uncertain = c->failed = 1; error = ERR_UNSPEC; break;
            }
            int32_t status; memcpy(&status, dctx->completed_rsp + 4, sizeof status);
            if (!status && req.fence > q->completed) q->completed = req.fence;
            else if (status && status != -ETIMEDOUT && status != -EDEADLK) {
                q->uncertain = c->failed = 1;
            }
            /* A transport ACK is not a fence signal: native ret is untouched. */
            return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
        }
        if (u32(p) != MSM_CCMD_IOCTL_SIMPLE || len < 20) break;
        uint32_t ioctl_cmd = u32(p + 16);
        struct native_queue *q = NULL;
        size_t response_size;
        if (ioctl_cmd == DRM_IOCTL_MSM_GET_PARAM) {
            struct drm_msm_param param;
            if (len != 20 + sizeof param) break;
            memcpy(&param, p + 20, sizeof param);
            if (param.pipe != MSM_PIPE_3D0 || param.len || param.pad || param.value) break;
            /* Turnip must obtain the native context's disjoint VA slice, not
             * reuse the global capset range. Forward only properties actually
             * supplied by kgsl_msm_param; unknown queries must not become a
             * synthetic successful zero. Keep the native response untouched. */
            switch (param.param) {
            case MSM_PARAM_GPU_ID: case MSM_PARAM_CHIP_ID:
            case MSM_PARAM_GMEM_SIZE: case MSM_PARAM_GMEM_BASE:
            case MSM_PARAM_VA_START: case MSM_PARAM_VA_SIZE:
            case MSM_PARAM_PRIORITIES: case MSM_PARAM_HIGHEST_BANK_BIT:
            case MSM_PARAM_UCHE_TRAP_BASE:
                break;
            default:
                goto fail;
            }
            response_size = 8 + sizeof param;
        } else if (ioctl_cmd == DRM_IOCTL_MSM_SUBMITQUEUE_NEW) {
            struct drm_msm_submitqueue req;
            if (len != 20 + sizeof req) break;
            memcpy(&req, p + 20, sizeof req);
            if (req.flags || req.prio || req.id) break; /* bring-up: ordinary queue */
            for (unsigned i = 0; i < QUEUES; ++i) if (!c->queues[i].active) { q = &c->queues[i]; break; }
            if (!q) { error = ERR_MEMORY; break; }
            response_size = 8 + sizeof req;
        } else if (ioctl_cmd == DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE) {
            if (len != 24) break;
            q = submit_queue(c, u32(p + 20));
            if (!q || q->uncertain || q->submitted != q->completed) break;
            response_size = 8;
        } else break;
        if (u32(response) != response_size) break;
        /* submit_cmd receives the transport's aligned, private snapshot. The
         * native parser owns response bounds, ret/seqno and the actual ioctl. */
        if (execute_native(c, p, len)) { error = ERR_UNSPEC; break; }
        struct drm_context *dctx = to_drm_context(c->ctx);
        if (dctx->completed_rsp_len != response_size) { c->failed = 1; error = ERR_UNSPEC; break; }
        if (q && !u32(dctx->completed_rsp + 4)) {
            if (ioctl_cmd == DRM_IOCTL_MSM_SUBMITQUEUE_NEW) {
                struct drm_msm_submitqueue created; memcpy(&created, dctx->completed_rsp + 8, sizeof created);
                if (submit_queue(c, created.id)) { c->failed = 1; error = ERR_UNSPEC; break; }
                *q = (struct native_queue){.id = created.id, .active = 1};
            } else memset(q, 0, sizeof *q);
        }
        return answer(out, capacity, VGPU_RESP_OK, h.context, NULL, 0);
    }
    default: break;
    }
fail:
    if (n->rejections != UINT64_MAX) ++n->rejections;
    if (n->rejections <= 8 || !(n->rejections & (n->rejections - 1))) {
        unsigned contexts = 0, resources = 0, busy = 0, failed = 0;
        uint64_t bytes = 0;
        for (unsigned i = 0; i < CONTEXTS; ++i) if (n->contexts[i].id) {
            ++contexts; busy += context_busy(&n->contexts[i]); failed += n->contexts[i].failed != 0;
        }
        for (unsigned i = 0; i < RESOURCES; ++i) if (n->resources[i].id) {
            ++resources;
            bytes += n->resources[i].size;
        }
        fprintf(stderr, "GPU_NATIVE_REJECT op=0x%x context=%u error=0x%x "
                "contexts=%u/%u resources=%u/%u bytes=%" PRIu64 " count=%" PRIu64
                " busy=%u failed=%u peak=%u created=%" PRIu64 " destroyed=%" PRIu64 " capacity_rejections=%" PRIu64 "\n",
                h.type, h.context, error, contexts, CONTEXTS, resources, RESOURCES,
                bytes, n->rejections, busy, failed, n->peak, n->created, n->destroyed, n->capacity_rejections);
    }
    return answer(out, capacity, error, h.context, NULL, 0);
}
