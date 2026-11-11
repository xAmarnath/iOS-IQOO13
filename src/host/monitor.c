#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "../../include/linux/gunyah.h"

#ifndef VM_RAM_MIB
#define VM_RAM_MIB 512
#endif
#ifndef VM_VCPUS
#define VM_VCPUS 1
#endif
_Static_assert(VM_VCPUS == 1 || VM_VCPUS == 2, "Unsupported CPU count");
#define RAM_SIZE (UINT64_C(1048576) * VM_RAM_MIB)
_Static_assert(VM_RAM_MIB == 512 || VM_RAM_MIB == 2048 || VM_RAM_MIB == 3072 || VM_RAM_MIB == 5120, "Unsupported VM RAM profile");
_Static_assert(sizeof(size_t) == 8, "VM monitor needs 64-bit size_t");
#define GPA UINT64_C(0x80000000)
#define PAYLOAD_OFFSET (2u * 1024u * 1024u)
#define DTB_CAP (64u * 1024u)
#define REPORT_GPA UINT64_C(0x10000000)
#define UART_GPA UINT64_C(0x3f8)
#define RTC_GPA UINT64_C(0x2000)
static _Thread_local struct gunyah_vcpu_run *run_page;
static unsigned char uart[8], divisor[2];
static unsigned long serial_bytes, mmio_count;
static char serial_tail[1024];
static size_t tail_length;
static int boot_manager_seen;
static int proof_seen;
static int quiesced;
static int vm_started;
static uint64_t return_reason = UINT64_MAX;
static int loader_seen, darwin_seen, handoff_seen, fixture_failed;
static int heartbeat_count, heartbeat_started;
static double heartbeat_start, heartbeat_elapsed;
static double monitor_started;
static volatile sig_atomic_t expired;
static volatile sig_atomic_t forced_stop;
static volatile sig_atomic_t resume_requested;
static double run_deadline;
/* Keep lifetime I/O totals for diagnostics; bound each resumable segment. */
#define MMIO_SEGMENT_LIMIT 2000000UL
static unsigned long run_mmio_start;
#include "virtio-disks.h"
#include "target-owner-signals.h"
static struct block_device disks[4];
static unsigned disk_count;
#include "target-evidence.h"
#include "target-memory.h"
#include "target-diagnostic.h"
#include "target-touch.h"
#include "target-framebuffer.h"
#include "target-gpu.h"

/* Preserve UART lines when host capture/controller messages share stdout.
 * The single-core loop or SMP device mutex serializes these calls. Keep one
 * serial byte stream: a guest console flush may run on either vCPU.
 */
static unsigned char serial_output[2048];
static size_t serial_output_used;

static void serial_output_flush(void) {
    if (serial_output_used) {
        if (fwrite(serial_output, 1, serial_output_used, stdout) != serial_output_used)
            perror("serial output");
        serial_output_used = 0;
    }
}

static void serial_char(unsigned char c) {
                serial_output[serial_output_used++] = c;
                if (c == '\n' || serial_output_used == sizeof(serial_output))
                    serial_output_flush();
                ++serial_bytes;
                if (disk_count == 4) {
                    target_serial_char(c);
                    return;
                }
                if (tail_length == sizeof(serial_tail) - 1) {
                    memmove(serial_tail, serial_tail + 512, tail_length - 512);
                    tail_length -= 512;
                }
                serial_tail[tail_length++] = (char)c;
                serial_tail[tail_length] = 0;
                if (strstr(serial_tail, "BdsDxe: starting") || strstr(serial_tail, "UEFI Interactive Shell") ||
                    strstr(serial_tail, "Boot Manager")) boot_manager_seen = 1;
                if (strstr(serial_tail, "== XNULOADER v3 ==")) loader_seen = 1;
                if (strstr(serial_tail, "Darwin Kernel Version 22.5.0")) darwin_seen = 1;
                if (strstr(serial_tail, "I am process 1, uid 0, console fd 0.")) handoff_seen = 1;
                if (strstr(serial_tail, "XNU-PROBE: STOP")) fixture_failed = 1;
                if (c == '\n') {
                    char *line = serial_tail + tail_length - 1;
                    while (line > serial_tail && line[-1] != '\n') --line;
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    double seconds = now.tv_sec + now.tv_nsec / 1e9;
                    if (!heartbeat_started && strstr(line, "XNU-HEARTBEAT: START")) {
                        heartbeat_started = 1;
                        heartbeat_start = seconds;
                        fprintf(stderr, "\nTIMING userspace_started=%.3f\n", seconds - monitor_started);
                    }
                    int beat;
                    if (sscanf(line, "XNU-HEARTBEAT: %d", &beat) == 1) {
                        if (!heartbeat_started || beat != heartbeat_count + 1) fixture_failed = 1;
                        else heartbeat_count = beat;
                    }
                    if (strstr(line, "XNU-HEARTBEAT: PASS")) {
                        heartbeat_elapsed = seconds - heartbeat_start;
                        proof_seen = loader_seen && darwin_seen && handoff_seen &&
                            heartbeat_count == 20 && heartbeat_elapsed >= 18.0;
                        if (!proof_seen) fixture_failed = 1;
                    }
                }
}

/* Emulated registers. Optional GPU requests use the isolated native service. */
static int emulate_mmio(void) {
    uint64_t address = run_page->mmio.phys_addr;
    unsigned length = run_page->mmio.len;
    unsigned char *data = run_page->mmio.data;
    if (gpu.enabled && address >= VGPU_MMIO_BASE && address < VGPU_MMIO_BASE + 0x1000)
        return target_gpu_mmio(address, length, data, run_page->mmio.is_write);
    if (disk_count == 4 && (address == GD_POLL || address == GD_DONE))
        return gd_mmio(address, length, data, run_page->mmio.is_write);
    if (disk_count == 4 && (address == GT_POLL || address == GT_ACK ||
        (address >= GT_DATA && address < GT_DATA + sizeof(struct gt_frame))))
        return gt_mmio(address, length, data, run_page->mmio.is_write);
    if (address == REPORT_GPA + 8 && length == 8 && run_page->mmio.is_write) {
        memcpy(&return_reason, data, sizeof(return_reason));
        return 0;
    }
    if (address == REPORT_GPA && length == 4 && run_page->mmio.is_write) {
        uint32_t value;
        memcpy(&value, data, sizeof(value));
        /* XNU7 is the opt-in, single-vCPU target kernel panic path.
         * It can authorize terminal teardown, but never a successful boot. */
        if (value == UINT32_C(0x584e5537)) {
            if (return_reason != 1) return -1;
        } else if (value == UINT32_C(0x46575131)) { /* isolated firmware assertion */
            if (disk_count != 4 || return_reason != 2) return -1;
        } else if (value == UINT32_C(0x44425131)) { /* isolated loader failure */
            if (disk_count != 4 || return_reason != 3) return -1;
        } else if (value != UINT32_C(0x584e554c)) return -1;
#if VM_VCPUS == 2
        printf("\nSMP_SINGLE_CORE_REPORT reason=%" PRIu64 " terminal_authorized=0\n", return_reason);
        return -1;
#else
        quiesced = 1;
        printf("\nGUEST_QUIESCED reason=%" PRIu64 "\n", return_reason);
        return 0;
#endif
    }
    if (address >= UART_GPA && address < UART_GPA + 8 && length == 1) {
        unsigned reg = (unsigned)(address - UART_GPA);
        if (run_page->mmio.is_write) {
            if (reg < 2 && (uart[3] & 0x80)) divisor[reg] = data[0];
            else if (reg == 0) {
                serial_char(data[0]);
            } else uart[reg] = data[0];
        } else {
            if (reg < 2 && (uart[3] & 0x80)) data[0] = divisor[reg];
            else if (reg == 5) data[0] = 0x60; /* transmitter empty, no input */
            else if (reg == 6) data[0] = 0xb0; /* CTS, DSR, DCD */
            else if (reg == 2) data[0] = 0xc1; /* FIFO, no pending interrupt */
            else if (reg == 0) data[0] = 0;
            else data[0] = uart[reg];
        }
        return 0;
    }
    if (address >= UINT64_C(0x09000000) && address < UINT64_C(0x09001000) && length == 4) {
        uint32_t value = 0;
        unsigned reg = address - UINT64_C(0x09000000);
        if (run_page->mmio.is_write) {
            memcpy(&value, data, 4);
            if (reg == 0) serial_char((unsigned char)value);
        } else {
            if (reg == 0x18) value = 0x90; /* TX empty, RX empty */
            memcpy(data, &value, 4);
        }
        return 0;
    }
    if (address >= RTC_GPA && address < RTC_GPA + 0x1000 && length == 4) {
        uint32_t value = address == RTC_GPA ? (uint32_t)time(NULL) : 0;
        if (!run_page->mmio.is_write) memcpy(data, &value, sizeof(value));
        return 0;
    }
    for (unsigned i = 0; i < disk_count; ++i)
        if (address >= disks[i].mmio_base && address < disks[i].mmio_base + 0x1000)
            return block_mmio(&disks[i], address, length, data, run_page->mmio.is_write);
    fprintf(stderr, "\nUnsupported MMIO address=0x%" PRIx64 " write=%u len=%u\n",
            address, run_page->mmio.is_write, length);
    return -1;
}

_Static_assert(sizeof(struct gunyah_userspace_memory_region) == 32, "UAPI layout");
_Static_assert(offsetof(struct gunyah_vcpu_run, mmio) == 16, "UAPI union layout");

static void deadline(int sig) {
    (void)sig;
    forced_stop = 1;
    expired = 1;
#if VM_VCPUS == 1
    if (run_page) run_page->immediate_exit = 1;
#endif
}

/* Called only between completed ioctl/emulation cycles. A signal inside the
 * phone driver's VCPU_RUN may lose its stack-local MMIO resume_data. Never
 * resume a signal-interrupted run; retain ownership for diagnosis instead. */
static __attribute__((unused)) int cooperative_deadline(double now) {
    if (forced_stop || expired || quiesced || fixture_failed)
        return 0;
    int operation_limit = mmio_count - run_mmio_start >= MMIO_SEGMENT_LIMIT;
    if (now < run_deadline && !operation_limit)
        return 0;
    expired = 1;
    printf("RUN_BOUNDARY reason=%s segment_mmio=%lu lifetime_mmio=%lu\n",
           operation_limit ? "mmio_budget" : "time_budget",
           mmio_count - run_mmio_start, mmio_count);
    return 1;
}

static int hold_resumable(int cooperative_stop) {
    return cooperative_stop && expired && !forced_stop && !quiesced && !fixture_failed;
}

static double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        deadline(0);
        return 0;
    }
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void arm_run_deadline(unsigned seconds) {
    run_mmio_start = mmio_count;
    run_deadline = monotonic_seconds() + seconds;
    /* Fallback only for an ioctl that never returns to a safe boundary. */
    alarm(seconds + 5);
}

/* Explicit opt-in, checked only after a completed MMIO cycle. Removing the
 * marker requests a cooperative hold at the next boundary; never a live close.
 * Keep the stuck-ioctl fallback and all terminal/forced-stop guards. */
static __attribute__((unused)) int continue_live_session(int directory, int cooperative_stop) {
    if (!hold_resumable(cooperative_stop) || directory < 0) return 0;
    int fd = openat(directory, "session-live", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    char value[2];
    int enabled = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        !(st.st_mode & 0022) && st.st_nlink == 1 && st.st_size == 1 &&
        read(fd, value, sizeof(value)) == 1 && value[0] == '1';
    close(fd);
    if (!enabled || !hold_resumable(cooperative_stop)) return 0;
    expired = 0;
    arm_run_deadline(30);
    puts("VM_LIVE_CONTINUING: renewed cooperative 30-second segment");
    return 1;
}

#if VM_VCPUS == 2
#include "target-smp-production.h"
#endif

static int operation(int fd, unsigned long cmd, void *arg, const char *name) {
    if (expired) { errno = EINTR; return -1; }
    printf("%s: calling\n", name);
    int r = ioctl(fd, cmd, arg);
    int saved = errno;
    if (r < 0) printf("%s: errno=%d (%s)\n", name, saved, strerror(saved));
    else printf("%s: result=%d\n", name, r);
    errno = saved;
    return r;
}

static void request_resume(int sig) {
    (void)sig;
    resume_requested = 1;
}

static size_t load(const char *path, unsigned char *dst, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    size_t n = fread(dst, 1, cap, f);
    int extra = fgetc(f);
    int bad = ferror(f);
    fclose(f);
    if (!n || extra != EOF || bad) { fprintf(stderr, "Invalid input size: %s\n", path); return 0; }
    return n;
}

/* Staged binary can release only clean cache from validated regular files.
 * This mode returns before OOM settings, RAM preparation or any VM descriptor. */
static int release_readonly_cache(int count, char **paths) {
    for (int i = 0; i < count; ++i) {
        struct block_device disk;
        if (block_open(&disk, paths[i], 0, 0)) return 1;
        int rc = posix_fadvise(disk.disk_fd, 0, 0, POSIX_FADV_DONTNEED);
        close(disk.disk_fd);
        if (rc) { errno = rc; perror("release clean image cache"); return 1; }
    }
    puts("READONLY_IMAGE_CACHE_RELEASED vm_created=0");
    return 0;
}

int main(int argc, char **argv) {
    if (target_owner_signals_prepare()) { perror("protect owner diagnostic signal"); return 2; }
    if (argc == 5 && !strcmp(argv[1], "--release-image-cache"))
        return release_readonly_cache(3, argv + 2);
    int dev = -1, vm = -1, vcpu = -1, result = 1, run_size = 0;
    int deadline_stopped = 0;
#if VM_VCPUS == 2
    struct smp_runtime smp = {0};
    struct smp_production smp_context = {0};
    int vcpu1 = -1, run_size1 = 0;
    struct gunyah_vcpu_run *run_page1 = NULL;
    (void)deadline_stopped;
#endif
    unsigned char *ram = MAP_FAILED;
    struct target_memory target_ram = {0};
    struct target_framebuffer framebuffer = {.pixels = MAP_FAILED, .directory = -1};
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 4 && argc != 7) { fprintf(stderr, "Usage: %s guest.dtb firmware.fd boot.img [os.img systemos.img data.img]\n", argv[0]); return 2; }
    /* A killed owner closes live Gunyah descriptors. Protect this process only,
     * before opening a VM; the runner still requires physical-memory headroom. */
    int oom_fd = open("/proc/self/oom_score_adj", O_RDWR | O_CLOEXEC);
    if (oom_fd < 0) { perror("open owner oom_score_adj"); return 2; }
    char oom_value[32] = {0};
    int oom_ok = write(oom_fd, "-1000", 5) == 5 && lseek(oom_fd, 0, SEEK_SET) == 0 &&
                 read(oom_fd, oom_value, sizeof(oom_value) - 1) > 0 &&
                 strcmp(oom_value, "-1000\n") == 0;
    close(oom_fd);
    if (!oom_ok) { fputs("VM owner OOM protection failed\n", stderr); return 2; }
    puts("VM_OWNER_OOM_SCORE_ADJ=-1000");
    struct sigaction sa = {.sa_handler = deadline};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) || sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL)) { perror("sigaction"); return 2; }
    struct sigaction resume_sa = {.sa_handler = request_resume};
    sigemptyset(&resume_sa.sa_mask);
    if (sigaction(SIGUSR1, &resume_sa, NULL)) { perror("sigaction resume"); return 2; }
    sigset_t resume_blocked, hold_wait_mask;
    sigemptyset(&resume_blocked);
    sigaddset(&resume_blocked, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &resume_blocked, &hold_wait_mask)) {
        perror("block active resume signal"); return 2;
    }
    sigdelset(&hold_wait_mask, SIGUSR1);
    struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core)) { perror("setrlimit core"); return 2; }
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    monitor_started = started.tv_sec + started.tv_nsec / 1e9;
    /* RAM preparation has its own bound; guest runtime starts after VM_START. */
    alarm(60);
    printf("Gunyah UEFI: %u vCPUs, %u MiB private RAM, %u KiB shared RAM, regular disk files (boot/OS/SystemOS read-only, Data writable)\n", (unsigned)VM_VCPUS, (unsigned)VM_RAM_MIB, SHARED_SIZE / 1024u);
    for (unsigned i = 0; i < 4; ++i) disks[i].disk_fd = -1;
    disk_count = argc == 7 ? 4 : 1;
    for (unsigned i = 0; i < disk_count; ++i) {
        if (block_open(&disks[i], argv[3 + i], BLOCK_GPA + i * 0x1000, i == 3)) {
            fputs("Each disk must be an existing, nonempty regular file, aligned to 512 bytes\n", stderr);
            goto cleanup;
        }
        struct stat current;
        if (fstat(disks[i].disk_fd, &current)) goto cleanup;
        for (unsigned j = 0; j < i; ++j) {
            struct stat previous;
            if (fstat(disks[j].disk_fd, &previous)) goto cleanup;
            if (current.st_dev == previous.st_dev && current.st_ino == previous.st_ino) {
                fputs("Duplicate disk backing files refused\n", stderr);
                goto cleanup;
            }
        }
    }
    shared = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { perror("mmap shared"); goto cleanup; }
    memset(shared, 0, SHARED_SIZE);
    if (madvise(shared, SHARED_SIZE, MADV_NOHUGEPAGE) || mlock(shared, SHARED_SIZE)) {
        perror("prepare shared"); goto cleanup;
    }
    if (disk_count == 4 && framebuffer_prepare(&framebuffer, argv[1])) {
        perror("prepare shared framebuffer"); goto cleanup;
    }
    printf("dtb=0x%" PRIx64 " pc=0x%" PRIx64 " report=0x%" PRIx64 "\n", GPA, GPA + PAYLOAD_OFFSET, REPORT_GPA);
    if (disk_count == 4) {
        for (unsigned i = 0; i < disk_count; ++i) {
            if (disks[i].writable) continue;
            int cache_rc = posix_fadvise(disks[i].disk_fd, 0, 0, POSIX_FADV_DONTNEED);
            if (cache_rc) { errno = cache_rc; perror("release clean image cache"); goto cleanup; }
        }
        puts("READONLY_IMAGE_CACHE_RELEASED");
        /* Reserve shared GPU pages (prefer large folios) before private RAM.
         * No VM exists and no guest GPU work can be submitted at this point. */
        if (disk_count == 4 && target_gpu_prepare(&gpu, framebuffer.directory)) {
            perror("prepare shared GPU before VM creation"); goto cleanup;
        }
        if (gpu.enabled) {
            int compact_fd = open("/proc/sys/vm/compact_memory", O_WRONLY | O_CLOEXEC);
            if (compact_fd < 0) { perror("open pre-private compaction"); goto cleanup; }
            ssize_t compact_result = write(compact_fd, "1", 1);
            int compact_errno = compact_result < 0 ? errno : EIO;
            close(compact_fd);
            if (compact_result != 1) {
                errno = compact_errno; perror("pre-private compaction"); goto cleanup;
            }
            puts("GPU_PRE_PRIVATE_COMPACTION passes=1 vm_created=0");
        }
        if (target_memory_prepare(&target_ram, RAM_SIZE, GPA, &expired)) {
            perror("prepare target RAM before VM creation"); goto cleanup;
        }
        ram = target_ram.ram;
        /* Private RAM is DONTFORK; fork only after both allocations are pinned.
         * Reverify both below after backend initialization and firmware loads. */
        if (target_gpu_activate(&gpu)) {
            perror("activate GPU after RAM reservation"); goto cleanup;
        }
    } else {
        ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ram == MAP_FAILED) { perror("mmap RAM"); goto cleanup; }
        memset(ram, 0, RAM_SIZE);
        if (madvise(ram, RAM_SIZE, MADV_NOHUGEPAGE)) { perror("madvise"); goto cleanup; }
    }
    size_t dtb_size = load(argv[1], ram, DTB_CAP);
    size_t payload_size = load(argv[2], ram + PAYLOAD_OFFSET, 4u * 1024u * 1024u);
    if (!dtb_size || !payload_size || dtb_size < 40 || memcmp(ram, "\xd0\x0d\xfe\xed", 4)) goto cleanup;
    if (payload_size != 4u * 1024u * 1024u ||
        memcmp(ram + PAYLOAD_OFFSET + 0x38, "ARM\x64", 4)) {
        fputs("Not the expected ARM64 firmware image\n", stderr); goto cleanup;
    }
    __builtin___clear_cache((char *)ram, (char *)ram + RAM_SIZE);
    printf("Loaded DTB=%zu bytes capacity=%u payload=%zu bytes\n", dtb_size, DTB_CAP, payload_size);
    if (expired) { fputs("Deadline expired before VM creation\n", stderr); goto cleanup; }
    if (disk_count == 4 && target_memory_verify(&target_ram)) {
        perror("target RAM changed before VM creation"); goto cleanup;
    }
    if (gpu.enabled && vgpu_shared_verify(gpu.host.shared, VGPU_MEMORY_SIZE)) {
        perror("shared GPU folios changed before VM creation"); goto cleanup;
    }
    if (disk_count == 4 && gd_prepare(argv[1])) { perror("prepare diagnostics"); goto cleanup; }
    dev = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
    if (dev < 0) { perror("open /dev/gunyah"); goto cleanup; }
    vm = operation(dev, GUNYAH_CREATE_VM, NULL, "CREATE_VM");
    if (vm < 0) goto cleanup;
    struct gunyah_userspace_memory_region mem = {
        .label = 0, .flags = GUNYAH_MEM_ALLOW_RWX,
        .guest_phys_addr = GPA, .memory_size = RAM_SIZE,
        .userspace_addr = (uintptr_t)ram,
    };
    /* Android's older UAPI uses a separate ioctl, not a map-memory flag.
     * Do not access ram after VM_START: the host loses access while lent.
     * Qualcomm gunyah-test-vmm's holding-cell uses LEND + RWX as well. */
    if (operation(vm, GH_VM_ANDROID_LEND_USER_MEM, &mem, "ANDROID_LEND_USER_MEM") < 0) goto cleanup;
    struct gunyah_userspace_memory_region dma_region = {
        .label = 1, .flags = GUNYAH_MEM_ALLOW_READ | GUNYAH_MEM_ALLOW_WRITE,
        .guest_phys_addr = SHARED_GPA, .memory_size = SHARED_SIZE,
        .userspace_addr = (uintptr_t)shared,
    };
    if (operation(vm, GH_VM_SET_USER_MEM_REGION, &dma_region, "SHARE_DMA_MEMORY") < 0) goto cleanup;
    if (disk_count == 4) {
        struct gunyah_userspace_memory_region fb_region = {
            .label = 2, .flags = GUNYAH_MEM_ALLOW_READ | GUNYAH_MEM_ALLOW_WRITE,
            .guest_phys_addr = FB_GPA, .memory_size = FB_SIZE,
            .userspace_addr = (uintptr_t)framebuffer.pixels,
        };
        if (operation(vm, GH_VM_SET_USER_MEM_REGION, &fb_region, "SHARE_FRAMEBUFFER_MEMORY") < 0) goto cleanup;
        printf("FRAMEBUFFER_SHARED gpa=0x%" PRIx64 " bytes=%u width=%u height=%u stride=%u\n",
               FB_GPA, FB_SIZE, FB_WIDTH, FB_HEIGHT, FB_STRIDE);
    }
    if (gpu.enabled) {
        struct gunyah_userspace_memory_region gpu_region = {
            .label = 3, .flags = GUNYAH_MEM_ALLOW_READ | GUNYAH_MEM_ALLOW_WRITE,
            .guest_phys_addr = VGPU_COMMAND_BASE, .memory_size = VGPU_MEMORY_SIZE,
            .userspace_addr = (uintptr_t)gpu.host.shared,
        };
        if (operation(vm, GH_VM_SET_USER_MEM_REGION, &gpu_region, "SHARE_GPU_MEMORY") < 0) goto cleanup;
        printf("GPU_MEMORY_SHARED commands=%uMiB renderer=%uMiB private_imports=disabled\n",
               VGPU_COMMAND_SIZE >> 20, VGPU_POOL_SIZE >> 20);
    }
    struct gunyah_vm_dtb_config dtb = {.guest_phys_addr = GPA, .size = DTB_CAP};
    if (operation(vm, GUNYAH_VM_SET_DTB_CONFIG, &dtb, "SET_DTB_CONFIG") < 0) goto cleanup;
    struct gunyah_vm_boot_context pc = {
        .reg = GUNYAH_VM_BOOT_CONTEXT_REG(REG_SET_PC, 0), .value = GPA + PAYLOAD_OFFSET,
    };
    if (operation(vm, GUNYAH_VM_SET_BOOT_CONTEXT, &pc, "SET_BOOT_PC") < 0) goto cleanup;
    struct gunyah_vm_boot_context sp = {
        .reg = GUNYAH_VM_BOOT_CONTEXT_REG(REG_SET_SP, 0), .value = GPA + RAM_SIZE,
    };
    if (operation(vm, GUNYAH_VM_SET_BOOT_CONTEXT, &sp, "SET_BOOT_SP") < 0) goto cleanup;
    struct gunyah_vm_boot_context x0 = {
        .reg = GUNYAH_VM_BOOT_CONTEXT_REG(REG_SET_X, 0), .value = GPA,
    };
    if (operation(vm, GUNYAH_VM_SET_BOOT_CONTEXT, &x0, "SET_BOOT_X0_DTB") < 0) goto cleanup;
    struct gunyah_fn_vcpu_arg cpu_arg = {.id = 0};
    struct gunyah_fn_desc fn = {
        .type = GUNYAH_FN_VCPU, .arg_size = sizeof(cpu_arg), .arg = (uintptr_t)&cpu_arg,
    };
    vcpu = operation(vm, GUNYAH_VM_ADD_FUNCTION, &fn, "ADD_VCPU_0");
    if (vcpu < 0) goto cleanup;
    run_size = operation(vcpu, GUNYAH_VCPU_MMAP_SIZE, NULL, "VCPU_MMAP_SIZE");
    if (run_size < (int)sizeof(*run_page) || run_size > 65536) goto cleanup;
    void *mapping = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (mapping == MAP_FAILED) { perror("mmap vCPU"); goto cleanup; }
    run_page = mapping;
#if VM_VCPUS == 2
    cpu_arg.id = 1;
    vcpu1 = operation(vm, GUNYAH_VM_ADD_FUNCTION, &fn, "ADD_VCPU_1");
    if (vcpu1 < 0) goto cleanup;
    run_size1 = operation(vcpu1, GUNYAH_VCPU_MMAP_SIZE, NULL, "VCPU1_MMAP_SIZE");
    if (run_size1 < (int)sizeof(*run_page1) || run_size1 > 65536) goto cleanup;
    mapping = mmap(NULL, run_size1, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu1, 0);
    if (mapping == MAP_FAILED) { perror("mmap vCPU1"); goto cleanup; }
    run_page1 = mapping;
#endif
    if (operation(vm, GUNYAH_VM_START, NULL, "VM_START") < 0) goto cleanup;
    vm_started = 1;
    if (expired) goto cleanup;
#if VM_VCPUS == 2
    alarm(0); /* The controller polls; it never signals a resumable worker. */
    smp_context.framebuffer = &framebuffer;
    smp_context.deadline = monotonic_seconds() + 180;
    smp.context = &smp_context;
    smp.emulate = smp_production_emulate;
    smp.poll = smp_production_poll;
    smp.poll_us = 4000;
    smp.cpu[0].fd = vcpu; smp.cpu[0].run = run_page;
    smp.cpu[1].fd = vcpu1; smp.cpu[1].run = run_page1;
    enum smp_result outcome = smp_execute(&smp);
    printf("SMP_RUNTIME outcome=%d joined=%d forced=%d\n", outcome, smp.joined, atomic_load(&smp.forced));
    if (!smp.joined) smp_production_hold(); /* Workers may still reference state. */
    for (unsigned i=0; i<2; ++i)
        printf("SMP_CPU id=%u exits=%lu error=%d terminal=%d\n", i, smp.cpu[i].exits, smp.cpu[i].error, smp.cpu[i].terminal);
    goto cleanup; /* Production has no two-core terminal-close proof. */
#else
    arm_run_deadline(disk_count == 4 ? 180 : 60);
run_guest:
    deadline_stopped = 0;
    while (!expired && !quiesced && !fixture_failed) {
        if (cooperative_deadline(monotonic_seconds())) {
            if (continue_live_session(framebuffer.directory, 1)) continue;
            deadline_stopped = 1;
            break;
        }
        if (ioctl(vcpu, GUNYAH_VCPU_RUN, NULL) < 0) {
            if (errno == EINTR && expired) break;
            perror("VCPU_RUN"); goto cleanup;
        }
        if (run_page->exit_reason == GUNYAH_VCPU_EXIT_STATUS) {
            printf("VM status=%u exit_type=%u\n", run_page->status.status, run_page->status.exit_info.type);
            goto cleanup;
        }
        if (run_page->exit_reason == GUNYAH_VCPU_EXIT_PAGE_FAULT) {
            printf("page fault address=0x%" PRIx64 " attempt=%d\n", (uint64_t)run_page->page_fault.phys_addr, run_page->page_fault.attempt);
            goto cleanup;
        }
        if (run_page->exit_reason != GUNYAH_VCPU_EXIT_MMIO) goto cleanup;
        ++mmio_count;
        if (emulate_mmio()) goto cleanup;
        run_page->mmio.resume_action = GUNYAH_VCPU_RESUME_HANDLED;
        if (disk_count == 4) {
            struct timespec frame_now;
            clock_gettime(CLOCK_MONOTONIC, &frame_now);
            framebuffer_stream(&framebuffer, frame_now.tv_sec + frame_now.tv_nsec / 1e9);
            if (framebuffer_capture(&framebuffer, frame_now.tv_sec + frame_now.tv_nsec / 1e9, 0))
                perror("frame capture"); /* Capture failure never tears down a live VM. */
        }
    }
    result = disk_count == 1 && proof_seen && quiesced && return_reason == 0 && disks[0].disk_reads ? 0 : 1;
#endif
cleanup:
    alarm(0);
    /* SMP reaches cleanup only after its workers have joined. */
    serial_output_flush();
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &finished);
    if (vm_started && framebuffer_capture(&framebuffer, finished.tv_sec + finished.tv_nsec / 1e9, 1))
        perror("final frame capture");
    printf("\nTIMING elapsed=%.3f deadline=%d\n",
           finished.tv_sec + finished.tv_nsec / 1e9 - monitor_started, (int)expired);
    if (disk_count == 4) target_report();
    if (disk_count == 1) printf("\nXNU_RESULT loader=%d kernel_banner=%d userspace=%d\n", loader_seen, darwin_seen, handoff_seen);
    printf("HEARTBEAT_RESULT count=%d elapsed=%.3f pass=%d\n", heartbeat_count, heartbeat_elapsed, proof_seen);
    printf("\nDISK_RESULT reads=%lu proof_seen=%d\n", disks[0].disk_reads, proof_seen);
    for (unsigned i = 0; i < disk_count; ++i)
        printf("BLOCK_OBSERVATIONS slot=%u reads=%lu writes=%lu flushes=%lu writable=%d\n",
               i, disks[i].disk_reads, disks[i].disk_writes, disks[i].disk_flushes, disks[i].writable);
    for (unsigned i = 0; i < disk_count; ++i) block_report(&disks[i], "hold_or_exit");
    printf("\nFIRMWARE_RESULT serial_bytes=%lu mmio_count=%lu boot_manager_seen=%d\n",
           serial_bytes, mmio_count, boot_manager_seen);
#if VM_VCPUS == 2
    if (vm_started) smp_production_hold();
    if (run_page1 && munmap(run_page1, run_size1)) { perror("munmap vCPU1"); result = 1; }
    if (vcpu1 >= 0 && close(vcpu1)) { perror("close vCPU1"); result = 1; }
#else
    if (vm_started && !quiesced) {
        /* Closing a live guest regressed on this host. Leave the stopped
         * vCPU and its memory owned by this detached process for diagnosis.
         * The host deadline must never SIGKILL this process. */
        alarm(0);
        if (run_page) run_page->immediate_exit = 1;
        printf("VM_HOLD_RESUMABLE=%d cooperative=%d forced=%d\n",
               hold_resumable(deadline_stopped), deadline_stopped, (int)forced_stop);
        puts("VM_HELD: guest did not quiesce; no descriptor or RAM teardown");
        /* Resume only cooperative time/operation stops; failures stay held.
         * Block SIGUSR1 during the check so requests cannot be lost. */
        for (;;) {
            if (resume_requested) {
                resume_requested = 0;
                if (hold_resumable(deadline_stopped)) {
                    expired = 0;
                    run_page->immediate_exit = 0;
                    arm_run_deadline(30);
                    puts("VM_RESUMING: additional 30-second deadline");
                    goto run_guest;
                }
                puts("VM_RESUME_REFUSED: hold was not a resumable deadline stop");
            }
            sigsuspend(&hold_wait_mask);
        }
    }
#endif
    printf("Cleanup beginning (deadline=%d)\n", (int)expired);
    if (run_page) {
        run_page->immediate_exit = 1;
        void *mapping = run_page;
        run_page = NULL;
        puts("munmap vCPU: calling");
        if (munmap(mapping, run_size)) { perror("munmap vCPU"); result = 1; }
        puts("munmap vCPU: returned");
    }
    if (vcpu >= 0) { puts("close vCPU: calling"); int c = close(vcpu); printf("close vCPU: %d\n", c); if (c) result = 1; }
    if (vm >= 0) { puts("close VM: calling"); int c = close(vm); printf("close VM: %d\n", c); if (c) result = 1; }
    if (dev >= 0) close(dev);
    if (target_gpu_release(&gpu)) { perror("release GPU after VM close"); result = 1; }
    if (gd_directory >= 0) { close(gd_directory); gd_directory = -1; }
    if (framebuffer_release(&framebuffer)) { perror("release framebuffer"); result = 1; }
    if (target_ram.reservation) {
        if (target_memory_release(&target_ram)) { perror("munmap target RAM"); result = 1; }
    } else if (ram != MAP_FAILED && munmap(ram, RAM_SIZE)) { perror("munmap RAM"); result = 1; }
    if (shared != MAP_FAILED && munmap(shared, SHARED_SIZE)) { perror("munmap shared"); result = 1; }
    for (unsigned i = 0; i < disk_count; ++i)
        if (disks[i].disk_fd >= 0) close(disks[i].disk_fd);
    alarm(0);
    printf("Cleanup complete; result=%d\n", result);
    return result;
}
