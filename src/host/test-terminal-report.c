/* Exercise the real MMIO handler without opening /dev/gunyah. */
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

static struct gunyah_vcpu_run test_run;
static int report(uint64_t offset, uint64_t value, unsigned size, int write) {
    memset(&test_run, 0, sizeof(test_run));
    run_page = &test_run;
    run_page->mmio.phys_addr = REPORT_GPA + offset;
    run_page->mmio.len = size;
    run_page->mmio.is_write = write;
    memcpy(run_page->mmio.data, &value, size);
    return emulate_mmio();
}

int main(void) {
    assert(report(0, 0x584e5537, 4, 1) == -1); /* no preceding reason */
    assert(!quiesced);
    assert(report(8, 0, 8, 1) == 0);
    assert(report(0, 0x584e5537, 4, 1) == -1); /* panic cannot claim success */
    assert(!quiesced);
    assert(report(8, 1, 8, 1) == 0);
    assert(report(0, 0x584e5537, 4, 1) == 0);
    assert(quiesced && return_reason == 1 && !proof_seen);
    quiesced = 0;
    assert(report(0, 0x12345678, 4, 1) == -1);
    assert(report(0, 0x584e5537, 4, 0) == -1);
    assert(report(0, 0x584e5537, 8, 1) == -1);
    assert(!quiesced);
    assert(report(8, 0, 8, 1) == 0);
    assert(report(0, 0x584e554c, 4, 1) == 0); /* old diagnostic retained */
    assert(quiesced && return_reason == 0 && !proof_seen);
    puts("TERMINAL_REPORT_TEST_PASS host_only=1");
    quiesced = 0;
    disk_count = 4;
    assert(report(0, 0x46575131, 4, 1) == -1); /* reason zero cannot pass */
    assert(report(8, 2, 8, 1) == 0);
    disk_count = 1;
    assert(report(0, 0x46575131, 4, 1) == -1); /* diagnostic cannot opt in */
    assert(!quiesced);
    disk_count = 4;
    assert(report(0, 0x46575131, 4, 1) == 0);
    assert(quiesced && return_reason == 2 && !proof_seen);
    puts("FIRMWARE_TERMINAL_FAILURE_TEST_PASS host_only=1");
    quiesced = 0;
    assert(report(0, 0x44425131, 4, 1) == -1); /* wrong reason */
    assert(report(8, 3, 8, 1) == 0);
    disk_count = 1;
    assert(report(0, 0x44425131, 4, 1) == -1);
    assert(!quiesced);
    disk_count = 4;
    assert(report(0, 0x44425131, 4, 0) == -1);
    assert(report(0, 0x44425131, 8, 1) == -1);
    assert(!quiesced);
    assert(report(0, 0x44425131, 4, 1) == 0);
    assert(quiesced && return_reason == 3 && !proof_seen);
    puts("LOADER_TERMINAL_FAILURE_TEST_PASS host_only=1");
    return 0;
}
