#ifndef TARGET_OWNER_SIGNALS_H
#define TARGET_OWNER_SIGNALS_H
#include <signal.h>

/* Android's BIONIC_SIGNAL_DEBUGGER is kernel signal 35 (32 + 3).
 * This static glibc monitor has no Bionic debuggerd handler. Default delivery
 * kills the VM owner and closes live descriptors. Ignore only this Android
 * diagnostic request; preserve crash, deadline and cooperative-stop signals.
 * Do not derive this from glibc SIGRTMIN, which has a different value.
 */
#define TARGET_ANDROID_DEBUGGER_SIGNAL 35
static int target_owner_signals_prepare(void) {
    struct sigaction ignore = {.sa_handler = SIG_IGN}, actual;
    if (sigemptyset(&ignore.sa_mask) ||
        sigaction(TARGET_ANDROID_DEBUGGER_SIGNAL, &ignore, NULL) ||
        sigaction(TARGET_ANDROID_DEBUGGER_SIGNAL, NULL, &actual)) return -1;
    return actual.sa_handler == SIG_IGN ? 0 : -1;
}
#endif
