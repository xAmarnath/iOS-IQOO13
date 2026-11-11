/* Temporarily permit mTHP allocation. Restore every changed policy before
 * VM creation, including cancellation and partial setup failures. */
#ifndef TARGET_THP_POLICY_H
#define TARGET_THP_POLICY_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "target-memory-config.h"

#define TARGET_POLICY_COUNT 9
struct target_thp_policy { char saved[TARGET_POLICY_COUNT][16]; unsigned touched; };
/* The last slot controls direct compaction for MADV_HUGEPAGE mappings. */
static const unsigned target_thp_kb[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 0};

static int target_policy_parse(const char *text, char selected[16]) {
    const char *begin = strchr(text, '['), *end = strchr(text, ']');
    if (!begin || !end || end <= begin || end - begin > 15 ||
            strchr(begin + 1, '[') || strchr(end + 1, ']')) { errno = EINVAL; return -1; }
    size_t n = (size_t)(end - begin - 1);
    memcpy(selected, begin + 1, n); selected[n] = 0;
    if (strcmp(selected, "always") && strcmp(selected, "inherit") &&
            strcmp(selected, "madvise") && strcmp(selected, "never") &&
            strcmp(selected, "defer") && strcmp(selected, "defer+madvise")) {
        errno = EINVAL; return -1;
    }
    return 0;
}

#ifndef TARGET_POLICY_IO_OVERRIDE
static void target_policy_path(unsigned slot, char path[128]) {
    if (!target_thp_kb[slot]) {
        snprintf(path, 128, "/sys/kernel/mm/transparent_hugepage/defrag");
        return;
    }
    snprintf(path, 128, "/sys/kernel/mm/transparent_hugepage/hugepages-%ukB/enabled",
             target_thp_kb[slot]);
}
static int target_policy_read(unsigned slot, char selected[16]) {
    char path[128], text[128]; target_policy_path(slot, path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, text, sizeof(text) - 1); } while (n < 0 && errno == EINTR);
    int saved = errno; close(fd); errno = saved;
    if (n <= 0) return -1;
    text[n] = 0;
    return target_policy_parse(text, selected);
}
static int target_policy_write(unsigned slot, const char *selected) {
    char path[128]; target_policy_path(slot, path);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t count = strlen(selected);
    ssize_t n;
    do { n = write(fd, selected, count); } while (n < 0 && errno == EINTR);
    int saved = errno; close(fd); errno = saved;
    if (n != (ssize_t)count) { if (n >= 0) errno = EIO; return -1; }
    return 0;
}
#endif

static int target_policy_restore(struct target_thp_policy *policy) {
    int rc = 0;
    for (unsigned i = 0; i < TARGET_POLICY_COUNT; ++i) {
        if (!(policy->touched & (1u << i))) continue;
        char value[16];
        if (target_policy_write(i, policy->saved[i]) || target_policy_read(i, value) ||
                strcmp(value, policy->saved[i])) {
            fprintf(stderr, "THP_POLICY_RESTORE_FAILED size_kb=%u saved=%s\n",
                    target_thp_kb[i], policy->saved[i]);
            rc = -1;
        } else {
            policy->touched &= ~(1u << i);
            printf("THP_POLICY_RESTORED size_kb=%u selected=%s\n", target_thp_kb[i], value);
        }
    }
    if (rc) errno = EIO;
    return rc;
}

static int target_policy_begin(struct target_thp_policy *policy) {
    /* Read all policies before changing any of them. */
    for (unsigned i = 0; i < TARGET_POLICY_COUNT; ++i)
        if (target_policy_read(i, policy->saved[i])) return -1;
    for (unsigned i = 0; i < TARGET_POLICY_COUNT; ++i) {
        const char *wanted = "madvise";
#ifdef TARGET_MAX_FOLIO_SIZE
        if (target_thp_kb[i] > TARGET_MAX_FOLIO_SIZE / 1024) wanted = "never";
#endif
        if (!strcmp(policy->saved[i], wanted)) continue;
        char value[16];
        policy->touched |= 1u << i; /* A failed write may still have changed state. */
        if (target_policy_write(i, wanted) || target_policy_read(i, value) ||
                strcmp(value, wanted)) {
            int saved = errno;
            if (target_policy_restore(policy)) return -1;
            errno = saved ? saved : EIO;
            return -1;
        }
    }
    return 0;
}
#endif
