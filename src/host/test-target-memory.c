#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int failure;
static volatile sig_atomic_t cancelled;
static int advise(void *p, size_t n, int advice) {
    if ((failure == 1 && advice == 23) || (failure == 2 && advice == 25)) {
        errno = ENOMEM; return -1;
    }
    if (failure == 4 && advice == 25) cancelled = 1;
    if (failure == 6 && advice == MADV_DONTFORK) { errno = EIO; return -1; }
    return madvise(p, n, advice);
}
static int lock_pages(const void *p, size_t n) {
    if (failure == 3) { errno = EPERM; return -1; }
    /* ASan's mlock interceptor is a no-op; exercise the real syscall. */
    return (int)syscall(SYS_mlock, p, n);
}
static int lock_onfault(const void *p, size_t n, unsigned flags) {
    if (failure == 5) { errno = EPERM; return -1; }
    assert(flags == MLOCK_ONFAULT);
    return (int)syscall(SYS_mlock2, p, n, flags);
}
#define madvise advise
#define mlock lock_pages
#define mlock2 lock_onfault
#include "target-memory.h"
#undef madvise
#undef mlock
#undef mlock2

static int coverage(const char *text) {
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    assert(f);
    int rc = target_memory_coverage(f, 0x200000, TARGET_FOLIO_SIZE);
    fclose(f);
    return rc;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct target_memory memory = {0};
    /* Run actual allocation, collapse, lock, smaps verification and release. */
    assert(!target_memory_prepare(&memory, TARGET_FOLIO_SIZE, 0x80000000, &cancelled));
    assert((uintptr_t)memory.ram % TARGET_FOLIO_SIZE == 0);
    assert(!target_memory_verify(&memory));
    unsigned char *old = memory.ram;
    for (size_t i = 0; i < memory.size; i += 4096) {
        assert(memory.ram[i] == 0); memory.ram[i] = 27;
    }
    /* Exercise the actual fork boundary used to start the GPU backend. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        unsigned char resident;
        int rc = mincore(memory.ram, 4096, &resident);
        _exit(rc == -1 && errno == ENOMEM ? 0 : 1);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!target_memory_verify(&memory));
    for (size_t i = 0; i < memory.size; i += 4096) assert(memory.ram[i] == 27);
    assert(!target_memory_release(&memory));
    unsigned char vector;
    assert(mincore(old, 4096, &vector) == -1 && errno == ENOMEM);
    assert(!memory.reservation && !memory.ram && !memory.size);
    for (failure = 1; failure <= 6; ++failure) {
        cancelled = 0;
        assert(target_memory_prepare(&memory, 2 * TARGET_FOLIO_SIZE, 0x80000000, &cancelled) == -1);
        assert(errno == (failure == 3 || failure == 5 ? EPERM : failure == 4 ? EINTR : failure == 6 ? EIO : ENOMEM));
        assert(!memory.reservation && !memory.ram);
    }
    failure = 0; cancelled = 1;
    assert(target_memory_prepare(&memory, TARGET_FOLIO_SIZE, 0x80000000, &cancelled) == -1 && errno == EINTR);
    cancelled = 0;
    assert(target_memory_prepare(&memory, TARGET_FOLIO_SIZE, 0x80001000, &cancelled) == -1 && errno == EINVAL);
    assert(target_memory_prepare(&memory, TARGET_FOLIO_SIZE - 4096, 0x80000000, &cancelled) == -1 && errno == EINVAL);
    assert(coverage("200000-400000 rw-p\nAnonHugePages: 2048 kB\nRss: 2048 kB\nLocked: 2048 kB\nSwap: 0 kB\n") == 0);
    assert(coverage("200000-400000 rw-p\nAnonHugePages: 1024 kB\nRss: 2048 kB\nLocked: 2048 kB\nSwap: 0 kB\n") == -1);
    assert(coverage("200000-400000 rw-p\nAnonHugePages: 2048 kB\nRss: 2048 kB\nLocked: 2048 kB\n") == -1);
    assert(coverage("200000-400000 rw-p\nAnonHugePages: 2048 kB\nRss: 2048 kB\nLocked: 2048 kB\nSwap: 4 kB\n") == -1);
    assert(coverage("200000-600000 rw-p\nAnonHugePages: 2048 kB\nRss: 2048 kB\nLocked: 2048 kB\nSwap: 0 kB\n") == -1);
    assert(coverage("") == -1);
    puts("TARGET_MEMORY_TEST_PASS real_allocation=1 refusal_paths=1 vm_created=0");
}
