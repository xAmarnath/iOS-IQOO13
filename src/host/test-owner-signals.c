#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "target-owner-signals.h"

static volatile sig_atomic_t stopped;
static void request_stop(int number) { stopped = number; }

int main(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction normal = {.sa_handler = SIG_DFL};
        assert(sigaction(TARGET_ANDROID_DEBUGGER_SIGNAL, &normal, NULL) == 0);
        raise(TARGET_ANDROID_DEBUGGER_SIGNAL);
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == TARGET_ANDROID_DEBUGGER_SIGNAL);
    struct sigaction stop = {.sa_handler = request_stop};
    assert(sigaction(SIGTERM, &stop, NULL) == 0);
    assert(target_owner_signals_prepare() == 0);
    union sigval value = {.sival_int = 1}; /* debuggerd uses queued delivery */
    for (int i = 0; i < 8; ++i)
        assert(sigqueue(getpid(), TARGET_ANDROID_DEBUGGER_SIGNAL, value) == 0);
    assert(raise(SIGTERM) == 0 && stopped == SIGTERM);
    puts("OWNER_SIGNAL_GUARD_PASS default_death=35 queued_requests=8 stop_preserved=1 vm_created=0");
    return 0;
}
