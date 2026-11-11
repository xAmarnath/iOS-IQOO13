/* Exercise the actual serial path without creating a VM. */
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

static void emit(const char *s) {
    while (*s) serial_char((unsigned char)*s++);
}

int main(void) {
    /* A host message during a partial UART line must not split that line. */
    setvbuf(stdout, NULL, _IONBF, 0);
    FILE *capture = tmpfile();
    assert(capture);
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    emit("droidvm-exc-meta tid=7 ");
    puts("HOST_FRAME");
    emit("cpu=1\n");
    /* Overlong and unterminated output must retain all bytes, including NUL. */
    for (unsigned i = 0; i < 5000; ++i) serial_char('x');
    serial_char(0);
    serial_output_flush();
    assert(serial_output_used == 0);
    const char expected[] = "HOST_FRAME\ndroidvm-exc-meta tid=7 cpu=1\n";
    assert(ftell(capture) == (long)(sizeof(expected) - 1 + 5001));
    rewind(capture);
    char prefix[sizeof(expected)] = {0};
    assert(fread(prefix, 1, sizeof(expected) - 1, capture) == sizeof(expected) - 1);
    assert(!strcmp(prefix, expected));
    for (unsigned i = 0; i < 5000; ++i) assert(fgetc(capture) == 'x');
    assert(fgetc(capture) == 0 && fgetc(capture) == EOF);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);
    memset(serial_tail, 0, sizeof(serial_tail));
    tail_length = 0;
    disk_count = 4;
    emit("BdsDxe: starting Boot0001\nASSERT [ConfigurationManagerDxe] SerialPortParser.c(196): 0\n");
    assert(boot_manager_seen && target_evidence.firmware_assert);
    emit("DARWINBOOT: hand-off to the kernel\n");
    assert(target_evidence.loader_handoff && !target_evidence.kernel_banner && !proof_seen);
    emit("== XNULOADER v3 ==\nDarwin Kernel Version 22.5.0: xnu-8796.101.5\n");
    emit("I am process 1, uid 0, console fd 0.\nXNU-HEARTBEAT: START\nXNU-HEARTBEAT: PASS\n");
    assert(!proof_seen && !fixture_failed && !target_evidence.kernel_banner);
    emit("Darwin Kernel Version 25.5.0: DEVELOPMENT_ARM64\n");
    emit("xnu-12377.121.6\n");
    assert(!target_evidence.kernel_banner); /* must be the same complete line */
    emit("Darwin Kernel Version 25.5.0: xnu-12377.");
    assert(!target_evidence.kernel_banner);
    emit("121.6/DEVELOPMENT_ARM64_BCM2837\n");
    assert(target_evidence.kernel_banner);
    emit("bsd_init enter\nvfs_mountroot mounted; authentication pending\n");
    emit("load_init_program enter\npanic(cpu 0 caller 0xffff): test\n");
    assert(target_evidence.bsd_init && target_evidence.root_mount);
    assert(target_evidence.init_attempt && target_evidence.panic);
    assert(!proof_seen && !quiesced);
    /* Captured 24A446 mount failure: initialization and selection happened,
       but entering APFS mountroot does not establish a mounted root or init. */
    memset(&target_evidence, 0, sizeof(target_evidence));
    emit("bsd_autoconf: calling kminit\r\n");
    emit("BSD root: disk3s1, major 1, minor 5\r\n");
    emit("apfs_vfsop_mountroot:3156: apfs: mountroot called!\r\n");
    emit("panic(cpu 0 caller 0xfffffff00b650750): fileset: original component called an unmapped kernel function\r\n");
    assert(target_evidence.bsd_init && target_evidence.root_selected && target_evidence.mount_attempt);
    assert(target_evidence.panic && !target_evidence.root_mount && !target_evidence.init_attempt);
    assert(!proof_seen && !quiesced);
    memset(&target_evidence, 0, sizeof(target_evidence));
    for (unsigned i = 0; i < 3000; ++i) serial_char('x');
    emit("bsd_init enter\n");
    assert(!target_evidence.bsd_init); /* overlong lines aren't reliable evidence */
    emit("bsd_init enter\n");
    assert(target_evidence.bsd_init);
    target_report();
    puts("TARGET_EVIDENCE_TEST_PASS host_only=1");
    return 0;
}
