#pragma once
#ifndef CRASH_RECORD_H
#define CRASH_RECORD_H

// Layout and validation of the record the hard fault handler leaves in backup SRAM.
// No Daisy/STM32 includes so tests/ can build it on a host machine.

#include <cstdint>
#include <cstring>

namespace bkshepherd {

constexpr uint32_t kCrashRecordMagic = 0xC7A5FA17u;

struct CrashRecord {
    uint32_t magic;    // kCrashRecordMagic when a record has been written
    uint32_t pc;       // program counter stacked by the fault
    uint32_t lr;       // link register stacked by the fault
    uint32_t cfsr;     // Configurable Fault Status Register (SCB->CFSR)
    int32_t effectID;  // activeEffectID at the time of the fault, -1 if unknown
    uint32_t uptimeMs; // milliseconds since boot
    uint32_t checksum; // CrashRecordChecksum over the fields above
};

static_assert(sizeof(CrashRecord) == 28, "CrashRecord layout is fixed; backup SRAM readers depend on it");

inline uint32_t CrashRecordChecksum(const CrashRecord &r) {
    uint32_t effect = static_cast<uint32_t>(r.effectID);
    return (r.magic ^ r.pc ^ r.lr ^ r.cfsr ^ effect ^ r.uptimeMs) ^ 0xA5A5A5A5u;
}

inline void CrashRecordFill(CrashRecord &r, uint32_t pc, uint32_t lr, uint32_t cfsr, int32_t effectID, uint32_t uptimeMs) {
    r.magic = kCrashRecordMagic;
    r.pc = pc;
    r.lr = lr;
    r.cfsr = cfsr;
    r.effectID = effectID;
    r.uptimeMs = uptimeMs;
    r.checksum = CrashRecordChecksum(r);
}

inline bool CrashRecordIsValid(const CrashRecord &r) {
    return r.magic == kCrashRecordMagic && r.checksum == CrashRecordChecksum(r);
}

inline void CrashRecordClear(CrashRecord &r) { std::memset(&r, 0, sizeof(r)); }

} // namespace bkshepherd

#endif
