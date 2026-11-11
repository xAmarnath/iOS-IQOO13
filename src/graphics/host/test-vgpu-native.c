/* Error injection against the production adapter; no GPU or phone access. */
#include <fcntl.h>
static int test_open(const char *path, int flags, ...);
#define open test_open
#include "vgpu-native.c"
#undef open
#include <assert.h>

static struct drm_context driver;
static struct vgpu_native *native;
static unsigned calls, detaches, destroys;
static int native_status, async_error, dispatch_error, bad_sequence, bad_length;
static uint32_t queue_id;
static int create_fails;
static void destroy(struct virgl_context *ctx);
static int test_open(const char *path, int flags, ...) {
    assert(!strcmp(path, KGSL_DEVICE_NODE));
    return open("/dev/null", flags);
}

int drm2kgsl_renderer_probe(int fd, struct virgl_renderer_capset_drm *cap) { abort(); }
struct virgl_context *drm2kgsl_renderer_create(int fd, size_t len, const char *name) {
    close(fd);
    if (create_fails) return NULL;
    struct drm_context *created=calloc(1,sizeof *created); assert(created);
    created->base.destroy=destroy;
    return &created->base;
}
static void detach(struct virgl_context *ctx, struct virgl_resource *res) { detaches++; }
static void destroy(struct virgl_context *ctx) { destroys++; if (ctx != &driver.base) free(ctx); }
static int submit(struct virgl_context *ctx, const void *bytes, size_t size) {
    const struct vdrm_ccmd_req *h = bytes;
    calls++;
    driver.completed_seqno = h->seqno - bad_sequence;
    driver.native_async_error = async_error;
    driver.completed_rsp_len = h->cmd == MSM_CCMD_GEM_SUBMIT ? 0 : 8;
    if (h->cmd == MSM_CCMD_IOCTL_SIMPLE && u32((char *)bytes + 16) == DRM_IOCTL_MSM_SUBMITQUEUE_NEW)
        driver.completed_rsp_len += sizeof(struct drm_msm_submitqueue);
    int is_param = h->cmd == MSM_CCMD_IOCTL_SIMPLE &&
        u32((char *)bytes + 16) == DRM_IOCTL_MSM_GET_PARAM;
    if (is_param) driver.completed_rsp_len = 8 + sizeof(struct drm_msm_param);
    uint32_t response[8] = {driver.completed_rsp_len, native_status, 0, 0, queue_id};
    if (is_param) {
        struct drm_msm_param param;
        memcpy(&param, (char *)bytes + 20, sizeof param);
        param.value = UINT64_C(0x123456789000);
        memcpy(response + 2, &param, sizeof param);
    }
    memcpy(driver.completed_rsp, response, sizeof response);
    if (bad_length) driver.completed_rsp_len = 0;
    /* Deliberately forge guest memory as successful and a different queue.
     * Only the host-owned native completion may govern GPU lifetimes. */
    uint32_t forged[] = {32, 0, 0, 0, 999};
    memcpy(native->pool + 64, forged, sizeof forged);
    return dispatch_error;
}
static void setup(void) {
    memset(&driver, 0, sizeof driver);
    driver.base.submit_cmd = submit; driver.base.detach_resource = detach; driver.base.destroy = destroy;
    native = calloc(1, sizeof *native); assert(native);
    native->pool = calloc(1, 131072); assert(native->pool);
    native->size = 131072; native->fd = -1;
    native->contexts[0] = (struct native_context){.id=1, .ctx=&driver.base, .has_shmem=1};
    native->live=native->peak=1; native->created=1;
    native->resources[0] = (struct native_resource){.id=1, .owner=1, .size=16384, .fd=-1, .mapped=1, .response_offset=64};
    native->resources[1] = (struct native_resource){.id=2, .owner=1, .size=65536, .offset=65536, .fd=-1, .blob=1, .iova=0x4000000000};
    calls=detaches=destroys=0; native_status=async_error=dispatch_error=bad_sequence=bad_length=create_fails=0; queue_id=7;
}
static uint32_t dispatch(uint32_t type, uint32_t ctx, const void *p, size_t len) {
    assert(len <= VGPU_REQUEST_MAX - sizeof(struct vgpu_header));
    unsigned char *wire=calloc(1,sizeof(struct vgpu_header)+len); assert(wire);
    struct vgpu_header out;
    struct vgpu_header h={.type=type, .context=ctx};
    memcpy(wire, &h, sizeof h);
    if (len) memcpy(wire+sizeof h, p, len);
    assert(vgpu_native_dispatch(native, 0, wire, sizeof h+len, &out, sizeof out)==sizeof out);
    free(wire); return out.type;
}
static uint32_t command(void *data, size_t len, uint32_t response_size) {
    unsigned char *payload=calloc(1,len+8); assert(payload);
    struct vdrm_ccmd_req *h=data;
    h->len=len; h->seqno=native->contexts[0].seqno+1;
    uint32_t prefix[]={len,0}; memcpy(payload,prefix,sizeof prefix); memcpy(payload+8,data,len);
    memcpy(native->pool+64,&response_size,4);
    uint32_t result=dispatch(0x207,1,payload,len+8); free(payload); return result;
}
static uint32_t ioctl_queue(int create) {
    uint32_t req[8]={MSM_CCMD_IOCTL_SIMPLE,0,0,0,
        create ? DRM_IOCTL_MSM_SUBMITQUEUE_NEW : DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE,0,0,0};
    if (!create) req[5]=queue_id;
    return command(req,create ? 32 : 24,create ? 20 : 8);
}
static uint32_t get_param(uint32_t id, uint32_t len, uint32_t pipe, uint64_t value) {
    unsigned char wire[20 + sizeof(struct drm_msm_param)] = {0};
    uint32_t cmd = MSM_CCMD_IOCTL_SIMPLE, ioctl = DRM_IOCTL_MSM_GET_PARAM;
    struct drm_msm_param param = {.pipe=pipe, .param=id, .len=len, .value=value};
    memcpy(wire,&cmd,4); memcpy(wire+16,&ioctl,4); memcpy(wire+20,&param,sizeof param);
    return command(wire,sizeof wire,8+sizeof param);
}
static uint32_t gpu_submit(void) {
    unsigned char wire[sizeof(struct msm_ccmd_gem_submit_req)+48]={0};
    struct msm_ccmd_gem_submit_req *r=(void *)wire;
    r->hdr.cmd=MSM_CCMD_GEM_SUBMIT; r->flags=MSM_PIPE_3D0|MSM_SUBMIT_FENCE_SN_IN|MSM_SUBMIT_NO_IMPLICIT;
    r->queue_id=queue_id; r->nr_bos=r->nr_cmds=1; r->fence=1;
    struct drm_msm_gem_submit_bo b={.flags=MSM_SUBMIT_BO_READ|MSM_SUBMIT_BO_WRITE,.handle=2,.presumed=0x4000000000};
    struct drm_msm_gem_submit_cmd c={.type=MSM_SUBMIT_CMD_BUF,.size=16};
    memcpy(r->payload,&b,sizeof b); memcpy(r->payload+sizeof b,&c,sizeof c);
    return command(wire,sizeof wire,0);
}
static uint32_t wait_fence(uint32_t fence) {
    struct msm_ccmd_wait_fence_req r={.hdr.cmd=MSM_CCMD_WAIT_FENCE,.queue_id=queue_id,.fence=fence};
    return command(&r,sizeof r,8);
}
static uint32_t unref(void) { uint32_t p[]={2,0}; return dispatch(0x102,0,p,sizeof p); }
static void finish(void) {
    /* Injection owns no real GPU; explicitly retire test state after all
     * retention assertions. Production never clears uncertainty this way. */
    memset(native->contexts[0].queues,0,sizeof native->contexts[0].queues);
    void *pool=native->pool; assert(!vgpu_native_destroy(native)); free(pool);
    assert(destroys==1);
}
static void resource_capacity(void) {
    setup();
    assert(RESOURCES > 64);
    native->size=VGPU_POOL_SIZE;
    native->cap.u.msm.va_start=UINT64_C(0x4000000000);
    native->cap.u.msm.va_size=UINT64_C(1)<<30;
    /* Model already-imported objects; device tests cover real udmabuf imports.
     * Exercise production lookup, exhaustion, retirement, and slot reuse. */
    for (unsigned i=1;i<RESOURCES;i++) {
        native->resources[i]=(struct native_resource){.id=i+1,.owner=1,
            .offset=VGPU_POOL_SIZE/2+i*16384,.size=16384,.fd=-1,.blob=i,
            .iova=native->cap.u.msm.va_start+i*16384};
    }
    struct msm_ccmd_gem_new_req gem={.hdr.cmd=MSM_CCMD_GEM_NEW,
        .iova=native->cap.u.msm.va_start+(256u<<20),.size=16384,
        .flags=MSM_BO_GUEST_ALLOC|MSM_BO_CACHED_COHERENT,.blob_id=RESOURCES};
    assert(command(&gem,sizeof gem,0)==ERR_MEMORY && calls==0);
    assert(!native->contexts[0].failed && !native->contexts[0].pending.blob_id);
    uint32_t last[]={RESOURCES,0};
    assert(dispatch(0x102,0,last,sizeof last)==VGPU_RESP_OK && detaches==1);
    assert(command(&gem,sizeof gem,0)==VGPU_RESP_OK && calls==1);
    assert(native->contexts[0].pending.blob_id==RESOURCES);
    /* Inject the resulting import: no GPU exists in this host harness. */
    native->resources[RESOURCES-1]=(struct native_resource){.id=RESOURCES,.owner=1,
        .offset=VGPU_POOL_SIZE/2+(RESOURCES-1)*16384,.size=16384,.fd=-1,
        .blob=gem.blob_id,.iova=gem.iova};
    memset(&native->contexts[0].pending,0,sizeof native->contexts[0].pending);
    assert(ioctl_queue(1)==VGPU_RESP_OK);
    enum { BO_COUNT=65 };
    size_t bytes=sizeof(struct msm_ccmd_gem_submit_req)+BO_COUNT*sizeof(struct drm_msm_gem_submit_bo)+sizeof(struct drm_msm_gem_submit_cmd);
    struct msm_ccmd_gem_submit_req *req=calloc(1,bytes); assert(req);
    req->hdr.cmd=MSM_CCMD_GEM_SUBMIT; req->queue_id=queue_id; req->fence=1;
    req->flags=MSM_PIPE_3D0|MSM_SUBMIT_FENCE_SN_IN|MSM_SUBMIT_NO_IMPLICIT;
    req->nr_bos=BO_COUNT; req->nr_cmds=1;
    for (unsigned i=0;i<BO_COUNT;i++) {
        struct drm_msm_gem_submit_bo bo={.flags=MSM_SUBMIT_BO_READ,
            .handle=i+2,.presumed=native->resources[i+1].iova};
        memcpy(req->payload+i*sizeof bo,&bo,sizeof bo);
    }
    struct drm_msm_gem_submit_cmd cmd={.type=MSM_SUBMIT_CMD_BUF,.submit_idx=BO_COUNT-1,.size=16};
    memcpy(req->payload+BO_COUNT*sizeof(struct drm_msm_gem_submit_bo),&cmd,sizeof cmd);
    assert(command(req,bytes,0)==VGPU_RESP_OK && context_busy(&native->contexts[0]));
    assert(dispatch(0x102,0,last,sizeof last)==VGPU_RESP_INVALID);
    assert(wait_fence(1)==VGPU_RESP_OK && !context_busy(&native->contexts[0]));
    req->fence=2; req->nr_bos=RESOURCES+1;
    unsigned before=calls;
    assert(command(req,bytes,0)==VGPU_RESP_INVALID && calls==before);
    free(req); finish();
    assert(detaches==RESOURCES);
    printf("CAPACITY PASS: %u slots, full-table rejection, retry after unref, 65-BO submit, high-slot retention and cleanup\n",RESOURCES);
}
static void context_capacity(void) {
    setup();
    vgpu_native_reset(native);
    assert(native->live==0 && native->destroyed==1);
    uint32_t req[18]={64,6};
    memset(req+2,'x',64);
    ((char *)(req+2))[0]='\n';
    assert(CONTEXTS==30);
    for (unsigned i=0;i<CONTEXTS;i++) {
        assert(dispatch(0x200,100+i,req,sizeof req)==VGPU_RESP_OK);
        assert(context(native,100+i)->name[0]=='?');
        assert(strlen(context(native,100+i)->name)==64);
    }
    assert(native->live==CONTEXTS && native->peak==CONTEXTS);
    assert(dispatch(0x200,999,req,sizeof req)==ERR_MEMORY);
    assert(native->capacity_rejections==1 && !context(native,999));
    assert(dispatch(0x200,100,req,sizeof req)==VGPU_RESP_INVALID);
    /* Busy/uncertain contexts must never be evicted to satisfy capacity. */
    context(native,100)->queues[0].uncertain=1;
    assert(dispatch(0x201,100,NULL,0)==VGPU_RESP_INVALID);
    assert(dispatch(0x200,999,req,sizeof req)==ERR_MEMORY);
    assert(native->live==CONTEXTS);
    context(native,100)->queues[0].uncertain=0; /* model retirement */
    assert(dispatch(0x201,100,NULL,0)==VGPU_RESP_OK);
    create_fails=1;
    assert(dispatch(0x200,999,req,sizeof req)==ERR_MEMORY);
    assert(native->live==CONTEXTS-1 && !context(native,999));
    create_fails=0;
    /* Repeated app restarts reuse slots; IDs and name lengths may change. */
    for (unsigned i=0;i<64;i++) {
        req[0]=i%2 ? 0 : 64;
        assert(dispatch(0x200,1000+i,req,sizeof req)==VGPU_RESP_OK);
        assert(strlen(context(native,1000+i)->name)==req[0]);
        assert(dispatch(0x201,1000+i,NULL,0)==VGPU_RESP_OK);
        assert(native->live==CONTEXTS-1);
    }
    vgpu_native_reset(native);
    assert(!native->live && native->created==native->destroyed);
    void *pool=native->pool;
    assert(vgpu_native_destroy(native)==0); free(pool);
    puts("NATIVE CONTEXT PASS: 30 concurrent, capacity, duplicate, busy retention, failed create, 64 restarts, reset balance");
}
int main(void) {
    context_capacity();
    resource_capacity();
    setup();
    for (unsigned i=0;i<2;i++) {
        assert(get_param(i ? MSM_PARAM_VA_SIZE : MSM_PARAM_VA_START,0,MSM_PIPE_3D0,0)==VGPU_RESP_OK);
        assert(u64(driver.completed_rsp+16)==UINT64_C(0x123456789000));
    }
    unsigned param_calls=calls;
    assert(get_param(UINT32_MAX,0,MSM_PIPE_3D0,0)==VGPU_RESP_INVALID);
    assert(get_param(MSM_PARAM_VA_START,8,MSM_PIPE_3D0,0)==VGPU_RESP_INVALID);
    assert(get_param(MSM_PARAM_VA_START,0,0,0)==VGPU_RESP_INVALID);
    assert(get_param(MSM_PARAM_VA_START,0,MSM_PIPE_3D0,1)==VGPU_RESP_INVALID);
    assert(calls==param_calls);
    native_status=-EINVAL;
    assert(get_param(MSM_PARAM_VA_SIZE,0,MSM_PIPE_3D0,0)==VGPU_RESP_OK);
    assert((int32_t)u32(driver.completed_rsp+4)==-EINVAL);
    finish();
    setup();
    assert(ioctl_queue(1)==VGPU_RESP_OK && submit_queue(&native->contexts[0],7));
    assert(!submit_queue(&native->contexts[0],999));
    assert(gpu_submit()==VGPU_RESP_OK && context_busy(&native->contexts[0]));
    unsigned previous=calls;
    assert(gpu_submit()==VGPU_RESP_INVALID && calls==previous);
    assert(wait_fence(2)==VGPU_RESP_INVALID && calls==previous);
    assert(unref()==VGPU_RESP_INVALID && !detaches);
    assert(dispatch(0x201,1,NULL,0)==VGPU_RESP_INVALID && !destroys);
    assert(ioctl_queue(0)==VGPU_RESP_INVALID && calls==previous);
    native_status=-ETIMEDOUT;
    assert(wait_fence(1)==VGPU_RESP_OK && context_busy(&native->contexts[0]));
    assert(native->contexts[0].queues[0].completed==0 && !native->contexts[0].failed);
    native_status=-EDEADLK;
    assert(wait_fence(1)==VGPU_RESP_OK && context_busy(&native->contexts[0]));
    native_status=0;
    assert(wait_fence(1)==VGPU_RESP_OK && !context_busy(&native->contexts[0]));
    native_status=-EIO; assert(ioctl_queue(0)==VGPU_RESP_OK);
    assert(submit_queue(&native->contexts[0],7)); /* driver close failure */
    native_status=0; assert(ioctl_queue(0)==VGPU_RESP_OK);
    assert(!submit_queue(&native->contexts[0],7));
    assert(unref()==VGPU_RESP_OK && detaches==1); finish();
    for (unsigned fault=0;fault<6;fault++) {
        setup(); assert(ioctl_queue(1)==VGPU_RESP_OK);
        if (fault==0) async_error=-EIO;
        if (fault==1) dispatch_error=-EIO;
        if (fault==2) bad_sequence=1;
        assert(gpu_submit()==(fault<3 ? ERR_UNSPEC : VGPU_RESP_OK));
        if (fault>=3) {
            native_status=fault==3 ? -EIO : -ETIMEDOUT;
            bad_length=fault==4;
            assert(wait_fence(1)==(bad_length ? ERR_UNSPEC : VGPU_RESP_OK));
        }
        assert(context_busy(&native->contexts[0]));
        vgpu_native_reset(native);
        assert(native->contexts[0].id==1 && native->contexts[0].failed);
        assert(native->resources[1].id==2 && !destroys && !detaches);
        assert(vgpu_native_destroy(native)==-EBUSY && !destroys && !detaches);
        assert(unref()==VGPU_RESP_INVALID);
        finish();
    }
    puts("NATIVE PASS: context VA queries, malformed query rejection, native errors, forged responses, timeout/retry, queue lifetime, reset retention (6 faults)");
}
