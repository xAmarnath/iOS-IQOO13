/* Serial observations only: none of these markers proves working userspace. */
static struct {
    int kernel_banner, bsd_init, root_selected, mount_attempt, root_mount, init_attempt, panic;
    int firmware_assert, loader_handoff;
    char line[2048];
    size_t used;
    int truncated;
} target_evidence;

static void target_serial_char(unsigned char c) {
    if (c != '\n') {
        if (target_evidence.used + 1 < sizeof(target_evidence.line))
            target_evidence.line[target_evidence.used++] = (char)c;
        else target_evidence.truncated = 1;
        return;
    }
    target_evidence.line[target_evidence.used] = 0;
    const char *line = target_evidence.line;
    if (!target_evidence.truncated) {
        if (strstr(line, "BdsDxe: starting") || strstr(line, "UEFI Interactive Shell"))
            boot_manager_seen = 1;
        if (strstr(line, "ASSERT [")) target_evidence.firmware_assert = 1;
        if (strstr(line, "DARWINBOOT: hand-off to the kernel"))
            target_evidence.loader_handoff = 1;
        if (strstr(line, "Darwin Kernel Version ") && strstr(line, "xnu-12377."))
            target_evidence.kernel_banner = 1;
        if (strstr(line, "bsd_init enter") || strstr(line, "bsd_autoconf: calling kminit"))
            target_evidence.bsd_init = 1;
        if (strncmp(line, "BSD root: ", 10) == 0) target_evidence.root_selected = 1;
        if (strncmp(line, "apfs_vfsop_mountroot:", 20) == 0 &&
            strstr(line, "apfs: mountroot called!")) target_evidence.mount_attempt = 1;
        if (strstr(line, "vfs_mountroot mounted; authentication pending"))
            target_evidence.root_mount = 1;
        if (strstr(line, "load_init_program enter")) target_evidence.init_attempt = 1;
        if (strstr(line, "panic(cpu ")) target_evidence.panic = 1;
    }
    target_evidence.used = 0;
    target_evidence.truncated = 0;
}

static void target_report(void) {
    printf("\nTARGET_FIRMWARE boot_manager=%d assertion=%d loader_handoff_announced=%d\n",
           boot_manager_seen, target_evidence.firmware_assert, target_evidence.loader_handoff);
    printf("\nTARGET_OBSERVATIONS xnu12377_banner=%d bsd_init=%d root_mount=%d init_attempt=%d panic=%d userspace_verified=0\n",
           target_evidence.kernel_banner, target_evidence.bsd_init,
           target_evidence.root_mount, target_evidence.init_attempt,
           target_evidence.panic);
    printf("TARGET_ROOT_OBSERVATIONS root_selected=%d mount_attempt=%d\n",
           target_evidence.root_selected, target_evidence.mount_attempt);
}
