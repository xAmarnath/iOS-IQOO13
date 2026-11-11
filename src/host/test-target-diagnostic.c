/* Exercise the actual monitor dispatch and disk-independent diagnostic path. */
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

static struct gunyah_vcpu_run test_run;
static uint32_t request_mmio(uint64_t address, uint32_t value, int writing) {
    memset(&test_run, 0, sizeof(test_run)); run_page = &test_run;
    test_run.mmio.phys_addr = address; test_run.mmio.len = 4;
    test_run.mmio.is_write = writing; memcpy(test_run.mmio.data, &value, 4);
    assert(emulate_mmio() == 0);
    memcpy(&value, test_run.mmio.data, 4); return value;
}
static void publish(struct gd_request r) {
    int fd = openat(gd_directory, "diagnostic.request", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0 && gd_transfer(fd, &r, sizeof(r), 1) == 0); assert(close(fd) == 0);
}
int main(void) {
    char path[] = "/tmp/gunyah-diagnostic-XXXXXX";
    assert(mkdtemp(path));
    char dtb[256]; snprintf(dtb, sizeof(dtb), "%s/guest.dtb", path);
    assert(gd_prepare(dtb) == 0);
    shared = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(shared != MAP_FAILED); memset(shared, 0xa5, SHARED_SIZE); disk_count = 4;
    assert(request_mmio(GD_POLL, 0, 0) == 0);
    struct gd_request r = {.magic=GD_MAGIC, .version=GD_VERSION, .sequence=1, .kind=GD_PROCESSES};
    struct gd_request bad = r; bad.kind = 5; publish(bad);
    assert(request_mmio(GD_POLL, 0, 0) == 0); /* never permit file writes */
    bad=r;bad.reserved[0]=1;publish(bad);assert(request_mmio(GD_POLL, 0, 0)==0);
    bad=r;bad.length=1;publish(bad);assert(request_mmio(GD_POLL, 0, 0)==0);
    bad=r;bad.version=1;publish(bad);assert(request_mmio(GD_POLL, 0, 0)==0);
    assert(unlinkat(gd_directory,"diagnostic.request",0)==0);
    assert(symlinkat("/dev/zero",gd_directory,"diagnostic.request")==0);
    assert(request_mmio(GD_POLL,0,0)==0);assert(unlinkat(gd_directory,"diagnostic.request",0)==0);
    assert(mkfifoat(gd_directory,"diagnostic.request",0600)==0);
    assert(request_mmio(GD_POLL,0,0)==0);assert(unlinkat(gd_directory,"diagnostic.request",0)==0);
    publish(r); assert(request_mmio(GD_POLL,0,0)==1);
    unsigned char *memory = shared + GD_GPA - SHARED_GPA;
    assert(!memcmp(memory,&r,sizeof(r)) && memory[sizeof(r)]==0);
    assert(shared[0xc3fff]==0xa5 && shared[0xe4000]==0xa5); /* no disk-DMA overlap */
    struct gd_request response=r; response.length=GD_CAPACITY+1;
    memcpy(memory,&response,sizeof(response));request_mmio(GD_DONE,1,1);
    assert(gd_active.sequence==1 && gd_completed==0 && !quiesced && !fixture_failed);
    response.length=8;response.total=1;memcpy(memory,&response,sizeof(response));
    memcpy(memory+sizeof(response),"29 1 SB\n",8);request_mmio(GD_DONE,1,1);
    assert(gd_completed==1 && gd_active.sequence==0 && !quiesced);
    int fd=openat(gd_directory,"diagnostic-00000001.response",O_RDONLY|O_NOFOLLOW);
    struct gd_request saved;assert(fd>=0 && gd_transfer(fd,&saved,sizeof(saved),0)==0);
    char bytes[8]; assert(gd_transfer(fd,bytes,sizeof(bytes),0)==0);close(fd);
    assert(saved.length==8 && !memcmp(bytes,"29 1 SB\n",8));
    assert(request_mmio(GD_POLL,0,0)==0); /* stale request cannot execute again */
    r.sequence=2;r.kind=GD_STACKSHOT;r.pid=29;publish(r);
    assert(request_mmio(GD_POLL,0,0)==2);
    response=r;response.total=1;response.length=2;memcpy(memory,&response,sizeof(response));
    request_mmio(GD_DONE,2,1);assert(gd_completed==1);
    response.status=5;response.length=0;response.total=0;memcpy(memory,&response,sizeof(response));
    request_mmio(GD_DONE,2,1);assert(gd_completed==2);
    request_mmio(GD_DONE,2,1);assert(gd_completed==2 && !quiesced);
    r.sequence=3;r.kind=GD_MEMORY;r.pid=63;r.offset=0x1000;r.length=8;
    assert(gd_request_valid(&r));
    bad=r;bad.pid=0;assert(!gd_request_valid(&bad));
    bad=r;bad.pid=-1;assert(!gd_request_valid(&bad));
    bad=r;bad.offset=0;assert(!gd_request_valid(&bad));
    bad=r;bad.offset=UINT64_C(1)<<47;assert(!gd_request_valid(&bad));
    bad=r;bad.offset=(UINT64_C(1)<<47)-7;assert(!gd_request_valid(&bad));
    bad.offset--;assert(gd_request_valid(&bad));
    bad=r;bad.length=0;assert(!gd_request_valid(&bad));
    bad=r;bad.length=GD_CAPACITY+1;assert(!gd_request_valid(&bad));
    publish(r);assert(request_mmio(GD_POLL,0,0)==3);
    response=r;response.total=7;memcpy(memory,&response,sizeof(response));
    request_mmio(GD_DONE,3,1);assert(gd_completed==2);
    response.total=8;response.length=7;memcpy(memory,&response,sizeof(response));
    request_mmio(GD_DONE,3,1);assert(gd_completed==2);
    response.length=8;memcpy(memory,&response,sizeof(response));
    memcpy(memory+sizeof(response),"memory!!",8);request_mmio(GD_DONE,3,1);
    assert(gd_completed==3 && !gd_active.sequence);
    fd=openat(gd_directory,"diagnostic-00000003.response",O_RDONLY|O_NOFOLLOW);
    assert(fd>=0 && gd_transfer(fd,&saved,sizeof(saved),0)==0);
    assert(gd_transfer(fd,bytes,sizeof(bytes),0)==0);close(fd);
    assert(saved.offset==0x1000 && saved.total==8 && saved.length==8 && !memcmp(bytes,"memory!!",8));
    r.sequence=4;publish(r);assert(request_mmio(GD_POLL,0,0)==4);
    response=r;response.status=EFAULT;response.length=0;response.total=1;
    memcpy(memory,&response,sizeof(response));request_mmio(GD_DONE,4,1);assert(gd_completed==3);
    response.total=0;memcpy(memory,&response,sizeof(response));request_mmio(GD_DONE,4,1);
    assert(gd_completed==4 && !gd_active.sequence && !quiesced);
    assert(shared[0xc3fff]==0xa5 && shared[0xe4000]==0xa5);
    unsigned char data[8]={0};
    assert(gd_mmio(GD_POLL,8,data,0)==-1 && gd_mmio(GD_POLL,4,data,1)==-1);
    assert(gd_mmio(GD_DONE,4,data,0)==-1);
    assert(unlinkat(gd_directory,"diagnostic.request",0)==0);
    assert(unlinkat(gd_directory,"diagnostic-00000001.response",0)==0);
    assert(unlinkat(gd_directory,"diagnostic-00000002.response",0)==0);
    assert(unlinkat(gd_directory,"diagnostic-00000003.response",0)==0);
    assert(unlinkat(gd_directory,"diagnostic-00000004.response",0)==0);
    close(gd_directory);gd_directory=-1;assert(rmdir(path)==0);
    assert(munmap(shared,SHARED_SIZE)==0);shared=MAP_FAILED;
    puts("GUNYAH_DIAGNOSTIC_TEST_PASS host_only=1");
}
