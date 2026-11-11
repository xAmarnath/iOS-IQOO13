/* Exercise the actual boundary/forced-stop policy without opening a VM. */
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

int main(void) {
    struct gunyah_vcpu_run page = {0};
    run_page = &page;
    run_deadline = 180;
    assert(!cooperative_deadline(179.99) && !expired);
    assert(cooperative_deadline(180) && expired);
    assert(hold_resumable(1) && !hold_resumable(0));
    assert(!page.immediate_exit); /* Normal boundary never interrupts VCPU_RUN. */
    deadline(SIGALRM);
    assert(page.immediate_exit && forced_stop && !hold_resumable(1));
    assert(!cooperative_deadline(181));
    for (int sig = 0; sig < 3; ++sig) {
        expired = forced_stop = 0;
        page.immediate_exit = 0;
        deadline(sig == 0 ? SIGALRM : sig == 1 ? SIGTERM : SIGINT);
        assert(!hold_resumable(1) && !cooperative_deadline(200));
    }
    expired = forced_stop = 0;
    quiesced = 1;
    assert(!cooperative_deadline(200) && !expired);
    quiesced = 0;
    fixture_failed = 1;
    assert(!cooperative_deadline(200) && !expired);
    fixture_failed = 0;
    assert(cooperative_deadline(200) && hold_resumable(1));
    /* Reproduce the lifetime-cap failure, then cross it on two resumes. */
    page.immediate_exit = 0;
    for (unsigned segment = 0; segment < 3; ++segment) {
        expired = 0;
        arm_run_deadline(30);
        alarm(0); /* This host test never executes VCPU_RUN. */
        double before_deadline = run_deadline - 1;
        unsigned long start = mmio_count;
        assert(run_mmio_start == start);
        assert(!cooperative_deadline(before_deadline));
        mmio_count += MMIO_SEGMENT_LIMIT - 1;
        assert(!cooperative_deadline(before_deadline));
        ++mmio_count;
        assert(cooperative_deadline(before_deadline));
        assert(hold_resumable(1) && !page.immediate_exit);
        assert(mmio_count == (segment + 1) * MMIO_SEGMENT_LIMIT);
    }
    /* Hitting the count never permits a failed/forced/terminal resume. */
    expired = 0;
    forced_stop = 1;
    assert(!cooperative_deadline(run_deadline) && !hold_resumable(1));
    forced_stop = 0;
    fixture_failed = 1;
    assert(!cooperative_deadline(run_deadline) && !hold_resumable(1));
    fixture_failed = 0;
    quiesced = 1;
    assert(!cooperative_deadline(run_deadline) && !hold_resumable(1));
    /* Live mode renews only genuine cooperative boundaries, and can be
     * disabled without a signal or closing the retained VM. */
    quiesced = expired = forced_stop = 0;
    char directory[] = "/tmp/xnu-live-session-XXXXXX";
    assert(mkdtemp(directory));
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dir >= 0);
    assert(cooperative_deadline(run_deadline));
    assert(!continue_live_session(dir, 1));
    int marker = openat(dir, "session-live", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(marker >= 0 && write(marker, "1", 1) == 1 && close(marker) == 0);
    for (unsigned i = 0; i < 3; ++i) {
        assert(continue_live_session(dir, 1));
        alarm(0);
        assert(!expired && !forced_stop && !page.immediate_exit);
        mmio_count += MMIO_SEGMENT_LIMIT;
        assert(cooperative_deadline(run_deadline - 1));
    }
    assert(!continue_live_session(dir, 0));
    forced_stop = 1; assert(!continue_live_session(dir, 1)); forced_stop = 0;
    fixture_failed = 1; assert(!continue_live_session(dir, 1)); fixture_failed = 0;
    quiesced = 1; assert(!continue_live_session(dir, 1)); quiesced = 0;
    assert(fchmodat(dir, "session-live", 0666, 0) == 0);
    assert(!continue_live_session(dir, 1));
    assert(unlinkat(dir, "session-live", 0) == 0);
    assert(!continue_live_session(dir, 1));
    assert(symlinkat("missing", dir, "session-live") == 0);
    assert(!continue_live_session(dir, 1));
    assert(unlinkat(dir, "session-live", 0) == 0);
    assert(mkfifoat(dir, "session-live", 0600) == 0);
    assert(!continue_live_session(dir, 1));
    assert(unlinkat(dir, "session-live", 0) == 0);
    assert(close(dir) == 0 && rmdir(directory) == 0);
    puts("RUN_DEADLINE_HOST_PASS cooperative_boundary forced_stop_nonresumable terminal_and_failure_guards repeated_mmio_segments");
}
