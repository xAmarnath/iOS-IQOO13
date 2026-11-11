// Actual guest worker with modeled IOKit, clock and MMIO; see tools/check-host.py.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "GunyahTouch.h"
using wait_result_t = int;
using kern_return_t = int;
using thread_t = int;
constexpr int KERN_SUCCESS = 0, kIOMapAnywhere = 1, kIOMapInhibitCache = 2;
constexpr uint64_t NSEC_PER_SEC = 1000000000;
static uint64_t tick;
static unsigned enabled = 1, cpus = 2, releases, starts, sleeps;
static bool map_fail, address_fail;
static int start_result;
static uint32_t registers[1024];
static std::vector<gt_frame> published;
static std::vector<std::string> logs;
static std::function<void()> on_sleep;
struct End {};
struct IOMemoryMap {
    uintptr_t getVirtualAddress() { return address_fail ? 0 : reinterpret_cast<uintptr_t>(registers); }
    void release() { ++releases; }
};
static IOMemoryMap mapping;
struct IODeviceMemory {
    static IODeviceMemory *withRange(uint64_t base, unsigned size) {
        assert(base == (GT_POLL & ~UINT64_C(0xfff)) && size == 0x1000);
        static IODeviceMemory device;
        return &device;
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags == (kIOMapAnywhere | kIOMapInhibitCache));
        return map_fail ? nullptr : &mapping;
    }
    void release() {}
};
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *absolute) { *absolute = ns; }
static uint64_t mach_absolute_time() { return tick; }
static void IOLog(const char *format, ...) {
    char line[256]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args); logs.emplace_back(line);
}
static bool PE_parse_boot_argn(const char *name, unsigned *value, size_t size) {
    assert(size == sizeof(unsigned)); *value = !strcmp(name, "cpus") ? cpus : enabled; return true;
}
static int kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *worker) {
    ++starts; *worker = 1; return start_result;
}
static void thread_deallocate(thread_t) {}
static void IOSleep(unsigned ms) { assert(ms == 8); ++sleeps; on_sleep(); }
extern "C" { volatile uint32_t native_touch_hid_reports; }
extern "C" void droidvm_gunyah_touch_publish(const gt_frame *frame) {
    published.push_back(*frame); native_touch_hid_reports = native_touch_hid_reports + 1;
}
#include "guest-worker-under-test.inc"

static unsigned count(const char *prefix) {
    unsigned n = 0; for (const auto &line : logs) n += line.rfind(prefix, 0) == 0; return n;
}
static void reset() {
    tick = 0; enabled = 1; cpus = 2; releases = starts = sleeps = 0;
    map_fail = address_fail = false; start_result = 0;
    gt_register_map = nullptr; gt_registers = registers;
    published.clear(); logs.clear(); memset(registers, 0, sizeof(registers));
}
static void frame(uint32_t seq, unsigned contacts = 1, bool valid = true) {
    gt_frame f = {}; f.magic = valid ? GT_MAGIC : 0; f.version = GT_VERSION;
    f.sequence = seq; f.count = contacts;
    if (contacts) f.contacts[0] = {1, 20, 30};
    memcpy(registers + (GT_DATA & 0xfff) / 4, &f, sizeof(f));
    registers[(GT_POLL & 0xfff) / 4] = seq;
}
static void run() { try { gt_worker(nullptr, 0); } catch (const End &) {} }
int main() {
    unsigned cases = 0;
    for (unsigned cores : {1u, 2u}) {
        reset(); cpus = cores; droidvm_gunyah_touch_start(); droidvm_gunyah_touch_start();
        assert(starts == 1 && count("GUNYAH_TOUCH_READY") == 1); ++cases;
    }
    reset(); cpus = 4; droidvm_gunyah_touch_start();
    assert(!starts && count("GUNYAH_TOUCH_UNSUPPORTED_CPUS") == 1); ++cases;
    reset(); enabled = 0; droidvm_gunyah_touch_start(); assert(!starts && logs.empty()); ++cases;
    reset(); map_fail = true; droidvm_gunyah_touch_start();
    assert(!starts && !gt_register_map && count("GUNYAH_TOUCH_MAP_FAILED") == 1); ++cases;
    reset(); address_fail = true; droidvm_gunyah_touch_start();
    assert(!starts && !gt_registers && !gt_register_map && releases == 1); ++cases;
    reset(); start_result = 5; droidvm_gunyah_touch_start();
    assert(!gt_registers && !gt_register_map && releases == 1);
    start_result = 0; droidvm_gunyah_touch_start(); assert(starts == 2 && gt_registers); ++cases;
    reset(); frame(1);
    on_sleep = [] {
        assert(registers[(GT_ACK & 0xfff) / 4] == sleeps);
        if (sleeps == 160) throw End{};
        tick += 8000000; frame(sleeps + 1);
    };
    run(); assert(published.size() == 160 && count("GUNYAH_TOUCH sequence=") == 1); ++cases;
    reset(); frame(1);
    on_sleep = [] {
        if (sleeps == 3) throw End{};
        tick = sleeps == 1 ? NSEC_PER_SEC - 1 : NSEC_PER_SEC;
        assert(published.size() == 1);
    };
    run(); assert(published.size() == 2 && published.back().count == 0);
    assert(count("GUNYAH_TOUCH_TIMEOUT_RELEASE") == 1); ++cases;
    reset(); frame(1);
    on_sleep = [] { if (sleeps == 2) throw End{}; tick += 1500000000; };
    run(); assert(published.size() == 2 && !published.back().count); ++cases;
    reset(); frame(1);
    on_sleep = [] {
        if (sleeps == 10) throw End{};
        tick += 200000000; frame(sleeps + 1);
    };
    run(); assert(published.size() == 10 && !count("GUNYAH_TOUCH_TIMEOUT_RELEASE")); ++cases;
    reset(); frame(1);
    on_sleep = [] {
        if (sleeps == 4) throw End{};
        tick += 500000000; frame(sleeps + 1, 1, false);
    };
    run(); assert(published.size() == 2 && !published.back().count);
    assert(count("GUNYAH_TOUCH_REJECTED") == 1 && registers[(GT_ACK & 0xfff) / 4] == 4); ++cases;
    reset(); frame(1);
    on_sleep = [] {
        if (sleeps == 3) throw End{};
        tick += 1500000000; frame(2, 0);
    };
    run(); assert(published.size() == 2 && !count("GUNYAH_TOUCH_TIMEOUT_RELEASE")); ++cases;
    reset(); frame(1);
    on_sleep = [] { if (sleeps == 3) throw End{}; tick += 100000000; };
    run(); assert(published.size() == 1); ++cases;
    printf("GUEST_TOUCH_WORKER_PASS cases=%u\n", cases);
}
