/* Actual MMIO path: boundaries, latched frames, ack ordering, malformed inputs. */
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>
static struct gunyah_vcpu_run test_run;
static int access_reg(uint64_t address, unsigned length, uint32_t *value, int writing) {
    memset(&test_run, 0, sizeof(test_run)); run_page = &test_run;
    test_run.mmio.phys_addr = address; test_run.mmio.len = length;
    test_run.mmio.is_write = writing; memcpy(test_run.mmio.data, value, 4);
    int rc = emulate_mmio(); memcpy(value, test_run.mmio.data, 4); return rc;
}
static uint32_t reg(uint64_t address, uint32_t value, int writing) {
    assert(!access_reg(address, 4, &value, writing)); return value;
}
static void publish(struct gt_frame frame) {
    unlinkat(gd_directory, "touch.request", 0);
    int fd = openat(gd_directory, "touch.request", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && !gd_transfer(fd, &frame, sizeof(frame), 1)); close(fd);
}
int main(void) {
    char path[] = "/tmp/gunyah-touch-XXXXXX"; assert(mkdtemp(path));
    char dtb[256]; snprintf(dtb, sizeof(dtb), "%s/guest.dtb", path);
    assert(!gd_prepare(dtb)); disk_count = 4;
    assert(reg(GT_POLL, 0, 0) == 0);
    struct gt_frame frame = {.magic=GT_MAGIC,.version=GT_VERSION,.sequence=1,.count=1,
        .contacts={{1,32767,0}}};
    struct gt_frame bad=frame;
    bad.count=11;publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.contacts[0].x=32768;publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.contacts[0].id=256;publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.count=2;bad.contacts[1]=bad.contacts[0];publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.contacts[9].y=1;publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.version=2;publish(bad);assert(!reg(GT_POLL,0,0));
    bad=frame;bad.sequence=0;publish(bad);assert(!reg(GT_POLL,0,0));
    assert(!unlinkat(gd_directory,"touch.request",0));
    assert(!symlinkat("/dev/zero",gd_directory,"touch.request"));assert(!reg(GT_POLL,0,0));
    assert(!unlinkat(gd_directory,"touch.request",0));
    assert(!mkfifoat(gd_directory,"touch.request",0600));assert(!reg(GT_POLL,0,0));
    publish(frame); assert(reg(GT_POLL,0,0)==1);
    bad=frame;bad.sequence=2;bad.count=0;memset(bad.contacts,0,sizeof(bad.contacts));publish(bad);
    assert(reg(GT_POLL,0,0)==1); // Active snapshot cannot be overwritten.
    for(unsigned i=0;i<sizeof(frame)/4;++i) {
        uint32_t expected;memcpy(&expected,(char*)&frame+i*4,4);
        assert(reg(GT_DATA+i*4,0,0)==expected);
    }
    uint32_t value=2;assert(access_reg(GT_ACK,4,&value,1)==-1 && gt_active.sequence==1);
    value=0;assert(access_reg(GT_DATA+1,4,&value,0)==-1);
    assert(access_reg(GT_POLL,8,&value,0)==-1);
    reg(GT_ACK,1,1);assert(gt_completed==1 && !gt_active.sequence);
    assert(reg(GT_POLL,0,0)==2);reg(GT_ACK,2,1);assert(!reg(GT_POLL,0,0));
    int fd=openat(gd_directory,"touch.ack",O_RDONLY);assert(fd>=0);
    assert(!gd_transfer(fd,&value,4,0) && value==2);close(fd);
    frame.sequence=3;publish(frame);
    struct timespec stale[2]={{1,0},{1,0}};
    assert(!utimensat(gd_directory,"touch.request",stale,0));assert(!reg(GT_POLL,0,0));
    struct timespec clock={100,100};
    assert(gt_recent((struct timespec){100,100},clock));
    assert(gt_recent((struct timespec){98,100},clock));
    assert(!gt_recent((struct timespec){98,99},clock));
    assert(!gt_recent((struct timespec){100,101},clock));
    assert(!gt_recent((struct timespec){101,0},clock));
    publish(frame);assert(!fchmodat(gd_directory,"touch.request",0644,0));
    assert(!reg(GT_POLL,0,0));
    publish(frame);
    assert(!linkat(gd_directory,"touch.request",gd_directory,"alias",0));
    assert(!reg(GT_POLL,0,0));assert(!unlinkat(gd_directory,"alias",0));
    if (!geteuid()) {
        assert(!fchownat(gd_directory,"touch.request",1,1,0));assert(!reg(GT_POLL,0,0));
        assert(!fchownat(gd_directory,"touch.request",0,0,0));
    }
    fd=openat(gd_directory,"touch.request",O_WRONLY);assert(fd>=0);
    assert(!ftruncate(fd,sizeof(frame)-1));close(fd);assert(!reg(GT_POLL,0,0));
    // A stale temporary file from the old writer must not block future ACKs.
    fd=openat(gd_directory,".touch-ack.tmp",O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);close(fd);
    publish(frame);assert(reg(GT_POLL,0,0)==3);reg(GT_ACK,3,1);
    assert(gt_completed==3 && !gt_pending_ack);
    assert(!unlinkat(gd_directory,".touch-ack.tmp",0));
    // A real failed rename: retain the consumed frame and retry, without
    // letting the guest publish it to HID twice or consume the next frame.
    frame.sequence=4;publish(frame);assert(reg(GT_POLL,0,0)==4);
    assert(!unlinkat(gd_directory,"touch.ack",0));
    assert(!mkdirat(gd_directory,"touch.ack",0700));
    reg(GT_ACK,4,1);assert(gt_completed==3 && gt_pending_ack==4 && gt_active.sequence==4);
    unsigned attempts=gt_ack_attempts;
    reg(GT_ACK,4,1);assert(gt_ack_attempts==attempts); // duplicate ACK is idempotent
    frame.sequence=5;publish(frame);assert(!reg(GT_POLL,0,0));
    value=5;assert(access_reg(GT_ACK,4,&value,1)==-1);
    assert(!unlinkat(gd_directory,"touch.ack",AT_REMOVEDIR));
    gt_ack_retry_at=0;assert(reg(GT_POLL,0,0)==5);
    assert(gt_completed==4 && !gt_pending_ack);
    fd=openat(gd_directory,"touch.ack",O_RDONLY);assert(fd>=0);
    assert(!gd_transfer(fd,&value,4,0) && value==4);close(fd);
    // Failed creation (invalid directory) also remains pending until recovery.
    int directory=gd_directory;gd_directory=-1;
    reg(GT_ACK,5,1);assert(gt_completed==4 && gt_pending_ack==5);
    gd_directory=directory;gt_ack_retry_at=0;assert(!reg(GT_POLL,0,0));
    assert(gt_completed==5 && !gt_pending_ack);
    reg(GT_ACK,5,1); // late duplicate completion
    // The transport saturates at UINT32_MAX; it never wraps into old IDs.
    frame.sequence=UINT32_MAX;publish(frame);assert(reg(GT_POLL,0,0)==UINT32_MAX);
    reg(GT_ACK,UINT32_MAX,1);frame.sequence=1;publish(frame);assert(!reg(GT_POLL,0,0));
    assert(!quiesced && !fixture_failed); // Touch cannot control VM lifetime.
    unlinkat(gd_directory,"touch.request",0);unlinkat(gd_directory,"touch.ack",0);
    close(gd_directory);assert(!rmdir(path));puts("TOUCH_MMIO_PASS");
}
