/* Actual production callbacks: reject single-core teardown proof under SMP. */
#define VM_VCPUS 2
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

int main(void) {
    struct target_framebuffer fb = {.directory=-1};
    struct smp_production context = {.framebuffer=&fb};
    struct gunyah_vcpu_run page = {.exit_reason=GUNYAH_VCPU_EXIT_MMIO};
    disk_count = 4;
    const uint32_t magic[] = {0x584e5537, 0x46575131, 0x44425131, 0x584e554c};
    const uint64_t reasons[] = {1, 2, 3, 0};
    for (unsigned i=0; i<4; ++i) {
        return_reason = reasons[i];
        page.mmio.phys_addr=REPORT_GPA; page.mmio.len=4; page.mmio.is_write=1;
        memcpy(page.mmio.data, &magic[i], 4);
        assert(smp_production_emulate(&context, i%2, &page)==SMP_STEP_ERROR);
        assert(!quiesced && !proof_seen);
    }
    disk_count=1;
    page.mmio.phys_addr=UART_GPA+5; page.mmio.len=1; page.mmio.is_write=0;
    assert(smp_production_emulate(&context, 1, &page)==SMP_STEP_CONTINUE);
    assert(page.mmio.data[0]==0x60 && page.mmio.resume_action==GUNYAH_VCPU_RESUME_HANDLED);
    page.exit_reason=GUNYAH_VCPU_EXIT_PAGE_FAULT;
    assert(smp_production_emulate(&context, 0, &page)==SMP_STEP_ERROR);
    context.deadline=smp_seconds()+60;
    assert(!smp_production_poll(&context));
    context.deadline=0;
    assert(smp_production_poll(&context));
    char directory[]="/tmp/smp-live-XXXXXX";
    assert(mkdtemp(directory));
    fb.directory=open(directory,O_DIRECTORY|O_RDONLY);
    assert(fb.directory>=0);
    int fd=openat(fb.directory,"session-live",O_CREAT|O_WRONLY,0600);
    assert(fd>=0 && write(fd,"1",1)==1);
    close(fd);
    assert(!smp_production_poll(&context));
    assert(context.deadline>smp_seconds()+25);
    context.deadline=0;
    assert(!fchmodat(fb.directory,"session-live",0666,0));
    assert(smp_production_poll(&context));
    assert(!unlinkat(fb.directory,"session-live",0));
    close(fb.directory);
    assert(!rmdir(directory));
    puts("SMP_PRODUCTION_CALLBACKS_PASS host_only=1");
    return 0;
}
