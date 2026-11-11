// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// A single bounded CP_MEM_WRITE in an otherwise canary-filled 64 KiB BO.
// No privileged register writes, relocations or shader code.
namespace DroidVMGpuProbe {
enum : uint32_t { Bytes = 65536, Words = Bytes / 4, Destination = 1024, Writes = 16, CommandWords = 3 + Writes };
inline uint32_t expected(uint32_t index, uint64_t iova, unsigned round) {
    if (index == 0) return 0x703d8012; // Type7 opcode0x3d,18 payload words, odd parity.
    if (index == 1) return uint32_t(iova + Destination * 4);
    if (index == 2) return uint32_t((iova + Destination * 4) >> 32);
    if (index >= 3 && index < CommandWords) return 0xc0830000u ^ (round << 8) ^ (index - 3);
    if (index >= Destination && index < Destination + Writes) return 0xc0830000u ^ (round << 8) ^ (index - Destination);
    return 0xc6000000u ^ index ^ round;
}
inline bool prepare(volatile uint32_t *words, uint64_t bytes, uint64_t iova, unsigned round) {
    if (!words || bytes != Bytes || (iova & 16383) || iova > UINT64_MAX - Bytes) return false;
    for (uint32_t i = 0; i < Words; ++i) words[i] = expected(i, iova, round);
    for (uint32_t i = 0; i < Writes; ++i) words[Destination + i] = 0xdead0000u ^ i;
    return true;
}
inline bool verify(const volatile uint32_t *words, uint64_t bytes, uint64_t iova, unsigned round, uint32_t &bad) {
    bad = UINT32_MAX;
    if (!words || bytes != Bytes || (iova & 16383) || iova > UINT64_MAX - Bytes) return false;
    for (uint32_t i = 0; i < Words; ++i) if (words[i] != expected(i, iova, round)) { bad = i; return false; }
    return true;
}
}
