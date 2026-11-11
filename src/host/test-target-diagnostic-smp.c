/* Exercise the actual SMP worker lock and production MMIO dispatch. */
#define VM_VCPUS 2
#define main unused_monitor_main
#include "monitor.c"
#undef main
#include <assert.h>

struct diagnostic_migration {
    atomic_int phase;
    unsigned polling_cpu, sequence, pending[2], noise[2];
    double deadline;
};

static int fake_run(struct smp_worker *cpu) {
    struct diagnostic_migration *test = cpu->runtime->context;
    unsigned id = cpu->id;
    struct gunyah_vcpu_run *page = cpu->run;
    if (test->pending[id] == 1) {
        uint32_t sequence;
        memcpy(&sequence, page->mmio.data, 4);
        assert(sequence == test->sequence);
        test->pending[id] = 0;
        atomic_store(&test->phase, 1);
    } else if (test->pending[id] == 2) {
        assert(gd_completed == test->sequence && !gd_active.sequence);
        test->pending[id] = 0;
        atomic_store(&test->phase, 2);
        atomic_store(&cpu->runtime->stop, 1);
    }
    memset(page, 0, sizeof(*page));
    page->exit_reason = GUNYAH_VCPU_EXIT_MMIO;
    int phase = atomic_load(&test->phase);
    if (phase == 0 && id == test->polling_cpu) {
        page->mmio.phys_addr = GD_POLL;
        page->mmio.len = 4;
        test->pending[id] = 1;
    } else if (phase == 1 && id != test->polling_cpu) {
        unsigned char *memory = shared + GD_GPA - SHARED_GPA;
        struct gd_request response;
        memcpy(&response, memory, sizeof(response));
        assert(response.sequence == test->sequence && gd_request_valid(&response));
        response.length = 8;
        response.total = 1;
        memcpy(memory, &response, sizeof(response));
        memcpy(memory + sizeof(response), "29 1 SB\n", 8);
        atomic_thread_fence(memory_order_seq_cst);
        page->mmio.phys_addr = GD_DONE;
        page->mmio.len = 4;
        page->mmio.is_write = 1;
        memcpy(page->mmio.data, &test->sequence, 4);
        test->pending[id] = 2;
    } else {
        page->mmio.phys_addr = UART_GPA + 5;
        page->mmio.len = 1;
        ++test->noise[id];
        usleep(10);
    }
    return 0;
}

static int bounded_poll(void *opaque) {
    struct diagnostic_migration *test = opaque;
    return smp_seconds() > test->deadline;
}

int main(void) {
    char path[] = "/tmp/gunyah-diagnostic-smp-XXXXXX", dtb[256];
    assert(mkdtemp(path));
    snprintf(dtb, sizeof(dtb), "%s/guest.dtb", path);
    assert(!gd_prepare(dtb));
    shared = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(shared != MAP_FAILED);
    memset(shared, 0xa5, SHARED_SIZE);
    disk_count = 4;
    for (unsigned seq = 1; seq <= 64; ++seq) {
        struct gd_request request = {.magic=GD_MAGIC, .version=GD_VERSION,
            .sequence=seq, .kind=GD_PROCESSES};
        int fd = openat(gd_directory, "diagnostic.request",
                         O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        assert(fd >= 0 && !gd_transfer(fd, &request, sizeof(request), 1));
        assert(!close(fd));
        struct diagnostic_migration test = {.polling_cpu=seq%2, .sequence=seq,
            .deadline=smp_seconds()+2};
        struct gunyah_vcpu_run pages[2] = {0};
        struct smp_runtime runtime = {.context=&test, .run_one=fake_run,
            .emulate=smp_production_emulate, .poll=bounded_poll, .poll_us=1000};
        runtime.cpu[0].run = &pages[0]; runtime.cpu[1].run = &pages[1];
        assert(smp_execute(&runtime) == SMP_HELD);
        assert(runtime.joined && !atomic_load(&runtime.forced));
        assert(atomic_load(&test.phase) == 2 && !quiesced && !fixture_failed);
        assert(!runtime.cpu[0].error && !runtime.cpu[1].error);
        assert(shared[0xc3fff] == 0xa5 && shared[0xe4000] == 0xa5);
        char name[80]; snprintf(name, sizeof(name), "diagnostic-%08u.response", seq);
        fd = openat(gd_directory, name, O_RDONLY | O_NOFOLLOW);
        struct gd_request saved; char payload[8];
        assert(fd >= 0 && !gd_transfer(fd, &saved, sizeof(saved), 0));
        assert(!gd_transfer(fd, payload, sizeof(payload), 0));
        assert(!close(fd));
        assert(saved.sequence == seq && saved.length == 8 && !memcmp(payload, "29 1 SB\n", 8));
        assert(!unlinkat(gd_directory, name, 0));
    }
    assert(!unlinkat(gd_directory, "diagnostic.request", 0));
    assert(!close(gd_directory)); gd_directory = -1;
    assert(!rmdir(path));
    assert(!munmap(shared, SHARED_SIZE)); shared = MAP_FAILED;
    puts("GUNYAH_DIAGNOSTIC_SMP_PASS migrations=64 host_only=1");
}
