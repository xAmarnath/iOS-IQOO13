#define _GNU_SOURCE
#define TARGET_MIXED_FOLIOS
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static char selected[9][16];
static int write_fail_slot = -1, restore_fail_slot = -1, read_fail_slot = -1;
static int target_policy_read(unsigned slot, char out[16]) {
    if ((int)slot == read_fail_slot) { errno = EIO; return -1; }
    strcpy(out, selected[slot]); return 0;
}
static int target_policy_write(unsigned slot, const char *value) {
    if ((int)slot == write_fail_slot) {
        write_fail_slot = -1;
        strcpy(selected[slot], value); errno = EIO; return -1;
    }
    if (strcmp(value, "madvise") && (int)slot == restore_fail_slot) {
        errno = EIO; return -1;
    }
    strcpy(selected[slot], value); return 0;
}
#define TARGET_POLICY_IO_OVERRIDE
#include "target-thp-policy.h"
static void reset(void) {
    for (unsigned i = 0; i < TARGET_POLICY_COUNT; ++i) strcpy(selected[i], "never");
    strcpy(selected[0], "never"); strcpy(selected[1], "inherit"); strcpy(selected[2], "always");
    strcpy(selected[7], "inherit");
    write_fail_slot = restore_fail_slot = read_fail_slot = -1;
}
static void unchanged(void) {
    assert(!strcmp(selected[0], "never") && !strcmp(selected[1], "inherit") && !strcmp(selected[2], "always"));
    for (unsigned i = 3; i < TARGET_POLICY_COUNT; ++i) assert(!strcmp(selected[i], i == 7 ? "inherit" : "never"));
}
int main(void) {
    char value[16];
    assert(!target_policy_parse("always inherit madvise [never]\n", value) && !strcmp(value, "never"));
    assert(target_policy_parse("always inherit madvise never", value) == -1);
    assert(target_policy_parse("[unknown]", value) == -1);
    assert(target_policy_parse("[never] [always]", value) == -1);
    assert(!target_policy_parse("always [defer+madvise] never", value) && !strcmp(value, "defer+madvise"));
    for (int slot = 0; slot < TARGET_POLICY_COUNT; ++slot) {
        reset(); struct target_thp_policy p = {0};
        read_fail_slot = slot;
        assert(target_policy_begin(&p) == -1 && p.touched == 0); unchanged();
        reset(); write_fail_slot = slot;
        assert(target_policy_begin(&p) == -1 && p.touched == 0); unchanged();
    }
    reset(); struct target_thp_policy p = {0};
    assert(!target_policy_begin(&p) && p.touched == (1u << TARGET_POLICY_COUNT) - 1);
    assert(!strcmp(selected[7], "never"));
    for (unsigned i = 0; i < TARGET_POLICY_COUNT; ++i)
        if (i != 7) assert(!strcmp(selected[i], "madvise"));
    assert(!target_policy_restore(&p) && !p.touched); unchanged();
    assert(!target_policy_begin(&p));
    restore_fail_slot = 1;
    assert(target_policy_restore(&p) == -1 && p.touched == 2);
    assert(!strcmp(selected[0], "never") && !strcmp(selected[2], "always"));
    restore_fail_slot = -1;
    assert(!target_policy_restore(&p)); unchanged();
    strcpy(selected[1], "madvise");
    assert(!target_policy_begin(&p) && p.touched == ((1u << TARGET_POLICY_COUNT) - 1 - 2));
    assert(!target_policy_restore(&p) && !strcmp(selected[1], "madvise"));
    puts("TARGET_POLICY_TEST_PASS partial_failure_restore=1 restore_failure_refused=1");
}
