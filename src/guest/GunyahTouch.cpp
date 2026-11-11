// SPDX-License-Identifier: MIT
// One input worker feeds the HID digitizer on the one- or two-vCPU board.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "GunyahTouch.h"
extern "C" void droidvm_gunyah_touch_publish(const struct gt_frame *);
extern "C" void droidvm_gunyah_touch_start(void);
extern "C" volatile uint32_t native_touch_hid_reports;
static IOMemoryMap *gt_register_map;
static volatile uint32_t *gt_registers;

static void gt_worker(void *, wait_result_t) {
    uint32_t last = 0;
    unsigned down = 0;
    uint64_t published = 0, log_time = 0, timeout = 0, log_interval = 0;
    uint64_t accepted = 0, rejected = 0;
    nanoseconds_to_absolutetime(NSEC_PER_SEC, &timeout);
    nanoseconds_to_absolutetime(UINT64_C(5) * NSEC_PER_SEC, &log_interval);
    for (;;) {
        const uint32_t sequence = gt_registers[(GT_POLL & 0xfff) / 4];
        if (sequence && sequence != last) {
            struct gt_frame frame;
            uint32_t words[sizeof(frame) / 4];
            for (unsigned i = 0; i < sizeof(frame) / 4; ++i)
                words[i] = gt_registers[(GT_DATA & 0xfff) / 4 + i];
            memcpy(&frame, words, sizeof(frame));
            if (frame.sequence == sequence && gt_valid(&frame)) {
                const uint32_t before = native_touch_hid_reports;
                droidvm_gunyah_touch_publish(&frame);
                down = frame.count;
                published = mach_absolute_time();
                ++accepted;
                if (accepted == 1 || published - log_time >= log_interval) {
                    IOLog("GUNYAH_TOUCH sequence=%u contacts=%u reports=%u accepted=%llu\n",
                          sequence, frame.count, native_touch_hid_reports - before,
                          (unsigned long long)accepted);
                    log_time = published;
                }
            } else {
                if (++rejected == 1 || !(rejected % 128))
                    IOLog("GUNYAH_TOUCH_REJECTED sequence=%u count=%llu\n", sequence,
                          (unsigned long long)rejected);
            }
            __asm__ volatile("dsb sy" ::: "memory");
            gt_registers[(GT_ACK & 0xfff) / 4] = sequence;
            last = sequence;
        }
        // Check on every iteration, including rejected frames. Scheduler stalls
        // must not extend a disconnected finger's lifetime by 125 more polls.
        if (down && mach_absolute_time() - published >= timeout) {
            // A disconnected viewer must never leave a finger pressed.
            struct gt_frame release = {GT_MAGIC, GT_VERSION, 1, 0, {}};
            droidvm_gunyah_touch_publish(&release);
            down = 0;
            IOLog("GUNYAH_TOUCH_TIMEOUT_RELEASE\n");
        }
        IOSleep(8);
    }
}

void droidvm_gunyah_touch_start(void) {
    unsigned enabled = 0, cpus = 0;
    PE_parse_boot_argn("droidvm-gunyah-touch", &enabled, sizeof(enabled));
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    // The producer is this single worker, independent of the number of CPUs.
    // Both host vCPU loops serialize MMIO access to the same latched frame.
    if (enabled != 1 || gt_register_map) return;
    if (cpus != 1 && cpus != 2) {
        IOLog("GUNYAH_TOUCH_UNSUPPORTED_CPUS cpus=%u\n", cpus);
        return;
    }
    IODeviceMemory *registers = IODeviceMemory::withRange(GT_POLL & ~UINT64_C(0xfff), 0x1000);
    if (registers) { gt_register_map = registers->map(kIOMapAnywhere | kIOMapInhibitCache); registers->release(); }
    if (!gt_register_map) { IOLog("GUNYAH_TOUCH_MAP_FAILED\n"); return; }
    gt_registers = reinterpret_cast<volatile uint32_t *>(gt_register_map->getVirtualAddress());
    if (!gt_registers) {
        gt_register_map->release(); gt_register_map = nullptr;
        IOLog("GUNYAH_TOUCH_ADDRESS_FAILED\n");
        return;
    }
    thread_t worker;
    kern_return_t status = kernel_thread_start(gt_worker, nullptr, &worker);
    if (status == KERN_SUCCESS) {
        thread_deallocate(worker);
        IOLog("GUNYAH_TOUCH_READY version=1 contacts=10 cpus=%u\n", cpus);
    } else {
        gt_register_map->release(); gt_register_map = nullptr;
        gt_registers = nullptr;
        IOLog("GUNYAH_TOUCH_THREAD_FAILED status=%d\n", status);
    }
}
