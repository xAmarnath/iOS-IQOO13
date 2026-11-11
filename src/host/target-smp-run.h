#ifndef TARGET_SMP_RUN_H
#define TARGET_SMP_RUN_H
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* Two independent RUN ioctls; all host device callbacks share one lock.
 * A returned ioctl is not terminal proof. Only a validated per-core terminal
 * trap authorizes that core's teardown. EINTR is never retried: this driver's
 * stack-local MMIO resume data may have been lost.
 *
 * STOP parks the cohort. This initial SMP runtime deliberately has no resume
 * operation. A HELD result requires retaining every VM/memory descriptor.
 */
enum smp_step { SMP_STEP_ERROR=-1, SMP_STEP_CONTINUE=0, SMP_STEP_TERMINAL=1 };
enum smp_result { SMP_HELD=0, SMP_ALL_TERMINAL=1 };
struct smp_runtime;
struct smp_worker {
    struct smp_runtime *runtime;
    unsigned id;
    int fd;
    struct gunyah_vcpu_run *run;
    pthread_t thread;
    atomic_int finished;
    int created, terminal, error;
    unsigned long exits;
};
struct smp_runtime {
    struct smp_worker cpu[2];
    pthread_mutex_t devices;
    atomic_int stop, forced;
    int joined;
    void *context;
    int (*run_one)(struct smp_worker *);
    enum smp_step (*emulate)(void *, unsigned, struct gunyah_vcpu_run *);
    /* Controller only; nonzero requests a cooperative cohort stop. */
    int (*poll)(void *);
    unsigned poll_us; /* Zero selects the default 10 ms controller cadence. */
};
static double smp_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void smp_interrupt(int number) { (void)number; }
static int smp_ioctl_run(struct smp_worker *cpu) {
    return ioctl(cpu->fd, GUNYAH_VCPU_RUN, NULL);
}
static void *smp_worker_main(void *argument) {
    struct smp_worker *cpu=argument;
    struct smp_runtime *runtime=cpu->runtime;
    /* Deadline/stop signals belong to the controller. Only its explicit
     * SIGUSR2 can interrupt RUN, and only after the cohort is nonresumable. */
    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGUSR2);
    int error=pthread_sigmask(SIG_SETMASK, &mask, NULL);
    if (error) { cpu->error=error; atomic_store(&runtime->stop, 1); goto finished; }
    while (!atomic_load(&runtime->stop)) {
        int result=runtime->run_one(cpu);
        int run_error=errno;
        pthread_mutex_lock(&runtime->devices);
        if (result<0) {
            cpu->error=run_error ? run_error : EIO;
            if (run_error==EINTR) atomic_store(&runtime->forced, 1);
            atomic_store(&runtime->stop, 1);
        } else {
            ++cpu->exits;
            enum smp_step step=runtime->emulate(runtime->context, cpu->id, cpu->run);
            if (step==SMP_STEP_TERMINAL) cpu->terminal=1;
            else if (step!=SMP_STEP_CONTINUE) {
                cpu->error=EPROTO;
                atomic_store(&runtime->stop, 1);
            }
        }
        pthread_mutex_unlock(&runtime->devices);
        if (cpu->terminal || cpu->error) break;
    }
finished:
    atomic_store_explicit(&cpu->finished, 1, memory_order_release);
    return NULL;
}
static int smp_finished(struct smp_runtime *runtime) {
    for (unsigned i=0;i<2;++i)
        if (runtime->cpu[i].created &&
            !atomic_load_explicit(&runtime->cpu[i].finished, memory_order_acquire)) return 0;
    return 1;
}
static enum smp_result smp_execute(struct smp_runtime *runtime) {
    if (!runtime->emulate || !runtime->poll) return SMP_HELD;
    if (runtime->poll_us && (runtime->poll_us < 1000 || runtime->poll_us > 10000)) return SMP_HELD;
    if (!runtime->run_one) runtime->run_one=smp_ioctl_run;
    if (pthread_mutex_init(&runtime->devices, NULL)) return SMP_HELD;
    struct sigaction action={.sa_handler=smp_interrupt};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, NULL)) {
        pthread_mutex_destroy(&runtime->devices); return SMP_HELD;
    }
    /* Block controller signals before creating workers, avoiding the startup
     * race before each worker installs its own mask. */
    sigset_t blocked, saved;
    sigfillset(&blocked);
    if (pthread_sigmask(SIG_SETMASK, &blocked, &saved)) {
        pthread_mutex_destroy(&runtime->devices); return SMP_HELD;
    }
    for (unsigned i=0;i<2;++i) {
        struct smp_worker *cpu=&runtime->cpu[i];
        cpu->runtime=runtime; cpu->id=i;
        int error=pthread_create(&cpu->thread, NULL, smp_worker_main, cpu);
        if (error) { cpu->error=error; atomic_store(&runtime->stop, 1); break; }
        cpu->created=1;
    }
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    while (!smp_finished(runtime) && !atomic_load(&runtime->stop)) {
        if (runtime->poll(runtime->context)) atomic_store(&runtime->stop, 1);
        else usleep(runtime->poll_us ? runtime->poll_us : 10000);
    }
    atomic_store(&runtime->stop, 1);
    /* Give completed MMIO cycles time to reach their boundary. No worker is
     * cancelled or detached. A blocked ioctl gets one nonresumable interrupt. */
    double end=smp_seconds()+1.0;
    while (!smp_finished(runtime) && smp_seconds()<end) usleep(1000);
    for (unsigned i=0;i<2;++i) {
        struct smp_worker *cpu=&runtime->cpu[i];
        if (cpu->created && !atomic_load_explicit(&cpu->finished, memory_order_acquire)) {
            atomic_store(&runtime->forced, 1);
            __atomic_store_n(&cpu->run->immediate_exit, 1, __ATOMIC_RELEASE);
            pthread_kill(cpu->thread, SIGUSR2);
        }
    }
    end=smp_seconds()+2.0;
    while (!smp_finished(runtime) && smp_seconds()<end) usleep(1000);
    if (!smp_finished(runtime)) return SMP_HELD; /* caller must retain runtime */
    int terminal=1;
    for (unsigned i=0;i<2;++i) {
        struct smp_worker *cpu=&runtime->cpu[i];
        if (cpu->created) pthread_join(cpu->thread, NULL);
        if (!cpu->created || !cpu->terminal || cpu->error) terminal=0;
    }
    runtime->joined=1;
    pthread_mutex_destroy(&runtime->devices);
    return terminal && !atomic_load(&runtime->forced) ? SMP_ALL_TERMINAL : SMP_HELD;
}
#endif
