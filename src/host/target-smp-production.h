#ifndef TARGET_SMP_PRODUCTION_H
#define TARGET_SMP_PRODUCTION_H
#include "target-smp-run.h"

struct smp_production {
    struct target_framebuffer *framebuffer;
    double deadline;
    double metrics_at, stream_max_ms, capture_max_ms;
    unsigned polls, slow_polls, capture_errors;
    uint64_t stream_sampled, stream_sent, stream_unchanged, stream_dropped;
};

/* Called under the shared device mutex; run_page is per-thread. */
static enum smp_step smp_production_emulate(void *opaque, unsigned cpu,
                                           struct gunyah_vcpu_run *page) {
    struct smp_production *context = opaque;
    run_page = page;
    if (page->exit_reason != GUNYAH_VCPU_EXIT_MMIO) {
        printf("SMP_EXIT cpu=%u reason=%u\n", cpu, page->exit_reason);
        if (page->exit_reason == GUNYAH_VCPU_EXIT_STATUS)
            printf("VM status=%u exit_type=%u\n", page->status.status, page->status.exit_info.type);
        if (page->exit_reason == GUNYAH_VCPU_EXIT_PAGE_FAULT)
            printf("page fault address=0x%" PRIx64 " attempt=%d\n", (uint64_t)page->page_fault.phys_addr, page->page_fault.attempt);
        return SMP_STEP_ERROR;
    }
    ++mmio_count;
    if (emulate_mmio()) return SMP_STEP_ERROR;
    page->mmio.resume_action = GUNYAH_VCPU_RESUME_HANDLED;
    (void)context;
    return SMP_STEP_CONTINUE;
}

/* Only the controller publishes shared scanout. Hashing/copying 12 MiB or
 * writing a capture must not hold the device mutex and stall both vCPUs.
 * The shared pixels already have concurrent guest writers under SMP; the
 * scanout ring owns its copy. No private RAM or worker counters are read. */
static int smp_production_poll(void *opaque) {
    struct smp_production *context = opaque;
    if (expired || forced_stop) return 1;
    if (disk_count == 4) {
        double now = smp_seconds();
        framebuffer_stream(context->framebuffer, now);
        double streamed = smp_seconds();
        if (framebuffer_capture(context->framebuffer, streamed, 0)) {
            int error = errno;
            if (++context->capture_errors == 1 || !(context->capture_errors % 64))
                printf("SMP_CAPTURE_ERROR count=%u errno=%d\n", context->capture_errors, error);
        }
        double captured = smp_seconds();
        double stream_ms = (streamed - now) * 1000, capture_ms = (captured - streamed) * 1000;
        if (stream_ms > context->stream_max_ms) context->stream_max_ms = stream_ms;
        if (capture_ms > context->capture_max_ms) context->capture_max_ms = capture_ms;
        ++context->polls;
        if (captured - now >= 1.0 / 120.0) ++context->slow_polls;
        if (!context->metrics_at) context->metrics_at = now;
        if (captured - context->metrics_at >= 5) {
            struct scanout_client *stream = &context->framebuffer->stream;
            printf("SMP_FRAME_TIMING polls=%u slow=%u stream_max_ms=%.3f capture_max_ms=%.3f capture_errors=%u\n",
                   context->polls, context->slow_polls, context->stream_max_ms,
                   context->capture_max_ms, context->capture_errors);
            printf("SMP_SCANOUT seconds=%.3f sampled=%" PRIu64 " published=%" PRIu64
                   " unchanged=%" PRIu64 " dropped=%" PRIu64 "\n",
                   captured-context->metrics_at, stream->sampled-context->stream_sampled,
                   stream->sent-context->stream_sent, stream->unchanged-context->stream_unchanged,
                   stream->dropped-context->stream_dropped);
            context->stream_sampled=stream->sampled; context->stream_sent=stream->sent;
            context->stream_unchanged=stream->unchanged; context->stream_dropped=stream->dropped;
            context->metrics_at = captured;
            context->polls = context->slow_polls = 0;
            context->stream_max_ms = context->capture_max_ms = 0;
        }
    }
    if (smp_seconds() < context->deadline) return 0;
    int fd = openat(context->framebuffer->directory, "session-live",
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    char value[2];
    int enabled = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
        st.st_uid == geteuid() && !(st.st_mode & 0022) && st.st_nlink == 1 &&
        st.st_size == 1 && read(fd, value, sizeof(value)) == 1 && value[0] == '1';
    if (fd >= 0) close(fd);
    if (!enabled) return 1;
    context->deadline = smp_seconds() + 30;
    puts("VM_LIVE_CONTINUING: SMP controller renewed 30-second segment");
    return 0;
}

static _Noreturn void smp_production_hold(void) {
    alarm(0);
    puts("VM_HOLD_RESUMABLE=0 cooperative=0 forced=1");
    puts("VM_HELD: SMP has no cohort terminal proof; retain all owners and mappings");
    for (;;) pause();
}
#endif
