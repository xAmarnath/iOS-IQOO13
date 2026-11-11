// SPDX-License-Identifier: MIT
// Opt-in guest-mediated read-only diagnostics; no lifetime control.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "GunyahDiagnostic.h"

extern "C" int droidvm_export_diagnostic(uint32_t, int, uint64_t, void *, uint64_t, uint64_t *, uint64_t *);
extern "C" void droidvm_gunyah_diagnostic_start(void);
static IOMemoryMap *gd_memory_map, *gd_register_map;
static unsigned char *gd_memory;
static volatile uint32_t *gd_registers;

static void gd_worker(void *, wait_result_t) {
    uint32_t last = 0;
    for (;;) {
        const uint32_t sequence = gd_registers[0];
        __asm__ volatile("dmb sy" ::: "memory");
        if (sequence && sequence != last) {
            struct gd_request request;
            memcpy(&request, gd_memory, sizeof(request));
            if (request.sequence == sequence && gd_request_valid(&request)) {
                const uint64_t capacity = request.kind == GD_MEMORY ? request.length : GD_CAPACITY;
                request.status = droidvm_export_diagnostic(request.kind, request.pid, request.offset,
                    gd_memory + sizeof(request), capacity, &request.length, &request.total);
                if (request.status) request.length = request.total = 0;
                memcpy(gd_memory, &request, sizeof(request));
                __asm__ volatile("dsb sy" ::: "memory");
                gd_registers[1] = sequence;
                __asm__ volatile("dsb sy" ::: "memory");
                last = sequence;
            }
        }
        IOSleep(100);
    }
}

void droidvm_gunyah_diagnostic_start(void) {
    unsigned enabled = 0, old_monitor = 0, cpus = 0;
    if (!PE_parse_boot_argn("droidvm-gunyah-diagnostic", &enabled, sizeof(enabled)) || enabled != 1) return;
    PE_parse_boot_argn("droidvm-monitor", &old_monitor, sizeof(old_monitor));
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    // The old mailbox shares scratch storage and must not run concurrently.
    // One guest worker owns the scratch buffer even when it migrates CPUs.
    // The SMP monitor serializes all MMIO under its shared device mutex.
    if (old_monitor || (cpus != 1 && cpus != 2) || gd_memory_map) {
        IOLog("GUNYAH_DIAGNOSTIC_REFUSED monitor=%u cpus=%u\n", old_monitor, cpus);
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(GD_GPA, GD_SIZE);
    IODeviceMemory *registers = IODeviceMemory::withRange(GD_POLL & ~UINT64_C(0xfff), 0x1000);
    if (memory) { gd_memory_map = memory->map(kIOMapAnywhere | kIOMapCopybackCache); memory->release(); }
    if (registers) { gd_register_map = registers->map(kIOMapAnywhere | kIOMapInhibitCache); registers->release(); }
    if (!gd_memory_map || !gd_register_map) {
        IOLog("GUNYAH_DIAGNOSTIC_MAP_FAILED\n");
        if (gd_memory_map) { gd_memory_map->release(); gd_memory_map = nullptr; }
        if (gd_register_map) { gd_register_map->release(); gd_register_map = nullptr; }
        return;
    }
    gd_memory = reinterpret_cast<unsigned char *>(gd_memory_map->getVirtualAddress());
    gd_registers = reinterpret_cast<volatile uint32_t *>(gd_register_map->getVirtualAddress() + (GD_POLL & 0xfff));
    thread_t worker;
    kern_return_t status = kernel_thread_start(gd_worker, nullptr, &worker);
    if (status == KERN_SUCCESS) {
        thread_deallocate(worker);
        IOLog("GUNYAH_DIAGNOSTIC_READY version=2 read_only=1 cpus=%u\n", cpus);
    } else {
        IOLog("GUNYAH_DIAGNOSTIC_THREAD_FAILED status=%d\n", status);
        gd_memory_map->release(); gd_memory_map = nullptr;
        gd_register_map->release(); gd_register_map = nullptr;
    }
}
