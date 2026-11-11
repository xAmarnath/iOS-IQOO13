#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include "../../include/linux/gunyah.h"
#include "target-smp-run.h"

struct scenario {
    unsigned mode, calls[2], emulated[2], inside, peak;
    double deadline;
    atomic_uint controller_waiting, progress[2];
    struct gunyah_vcpu_run pages[2];
};
static int mock_run(struct smp_worker *cpu) {
    struct scenario *test=cpu->runtime->context;
    unsigned call=++test->calls[cpu->id];
    if (test->mode==2 && cpu->id==1) { pause(); errno=EINTR; return -1; }
    if (test->mode==3 && cpu->id==1 && call==2) { errno=EINTR; return -1; }
    usleep(100);
    cpu->run->exit_reason=GUNYAH_VCPU_EXIT_MMIO;
    return 0;
}
static enum smp_step emulate(void *context, unsigned id, struct gunyah_vcpu_run *run) {
    struct scenario *test=context;
    ++test->inside;
    if (test->inside>test->peak) test->peak=test->inside;
    usleep(50);
    ++test->emulated[id];
    if (atomic_load(&test->controller_waiting)) atomic_fetch_add(&test->progress[id], 1);
    --test->inside;
    if (test->mode==4 && id==0 && test->emulated[id]==3) return SMP_STEP_ERROR;
    if (test->mode==1 || test->mode==3 || test->mode==5) {
        run->mmio.resume_action=GUNYAH_VCPU_RESUME_HANDLED;
        return SMP_STEP_CONTINUE;
    }
    if (test->emulated[id]==20) return SMP_STEP_TERMINAL;
    run->mmio.resume_action=GUNYAH_VCPU_RESUME_HANDLED;
    return SMP_STEP_CONTINUE;
}
static int poll_stop(void *context) {
    struct scenario *test=context;
    if (test->mode==5) {
        /* Model slow frame publication with a deterministic handshake: both
         * vCPUs must complete MMIO while the controller remains in poll(). */
        atomic_store(&test->controller_waiting, 1);
        double deadline=smp_seconds()+2;
        while (atomic_load(&test->progress[0])<4 || atomic_load(&test->progress[1])<4) {
            assert(smp_seconds()<deadline);
            usleep(1000);
        }
        return 1;
    }
    return smp_seconds()>test->deadline;
}
int main(void) {
    for (unsigned mode=0;mode<6;++mode) {
        struct scenario test={.mode=mode, .deadline=smp_seconds()+0.15};
        struct smp_runtime runtime={.context=&test, .run_one=mock_run,
            .emulate=emulate, .poll=poll_stop};
        for (unsigned i=0;i<2;++i) runtime.cpu[i].run=&test.pages[i];
        enum smp_result result=smp_execute(&runtime);
        assert(runtime.joined && test.peak==1 && test.inside==0);
        assert(result==(mode==0 ? SMP_ALL_TERMINAL : SMP_HELD));
        if (!mode) for (unsigned i=0;i<2;++i) assert(test.calls[i]==20 && test.emulated[i]==20);
        if (mode==1) assert(!atomic_load(&runtime.forced));
        if (mode==2) assert(atomic_load(&runtime.forced) && test.calls[1]==1 && runtime.cpu[0].terminal);
        if (mode==3) assert(atomic_load(&runtime.forced) && test.calls[1]==2 && test.emulated[1]==1);
        if (mode==4) assert(runtime.cpu[0].error==EPROTO);
        if (mode==5) assert(!atomic_load(&runtime.forced) && atomic_load(&test.progress[0])>=4 &&
                           atomic_load(&test.progress[1])>=4);
        printf("SMP_RUNTIME_PASS case=%u terminal=%d forced=%d\n",mode,result,atomic_load(&runtime.forced));
    }
    return 0;
}
