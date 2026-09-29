// Bump allocator that overrides libnosys' _sbrk() with a hard upper bound. See heap.h
// for why: an unbounded heap turns a leak into a random-looking bus fault far from its
// cause; this turns it into a crash record with cfsr 0x48454150 ('HEAP') instead.

#include "heap.h"

#include <cstddef>

#include "crash_handler.h"
#include "daisy_seed.h"

extern int activeEffectID; // defined in guitar_pedal.cpp

// Provided by the linker script: first free byte after .bss/.data, i.e. the bottom of
// the heap. Declared as an incomplete array type, not a single `char`, so the compiler
// cannot reason about pointer arithmetic off of it as if it were a real 1-byte object.
extern "C" char end[];

namespace {

// dependencies/libDaisy/core/STM32H750IB_sram.lds has no symbol marking the end of the
// RAM_D2 bank, so this is the constant: RAM_D2 origin 0x30008000 + 256 KB.
constexpr uintptr_t kHeapLimit = 0x30048000u;

// Lazily initialized to `end` on first call, per newlib's usual _sbrk contract.
char *s_brk = nullptr;
char *s_highWater = nullptr;

void ReportHeapExhaustedAndReset() {
    bkshepherd::CrashRecordFill(bkshepherd::g_crashRecord, 0, 0, 0x48454150u /* 'HEAP' */, activeEffectID, daisy::System::GetNow());
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(&bkshepherd::g_crashRecord), sizeof(bkshepherd::CrashRecord));
    __DSB();
    NVIC_SystemReset();
}

} // namespace

extern "C" void *_sbrk(ptrdiff_t incr) {
    if (s_brk == nullptr) {
        s_brk = end;
        s_highWater = end;
    }

    char *const base = s_brk;
    char *const requested = base + incr;

    // Newlib may ask to shrink the break (negative incr); never let it walk back past
    // the heap's start.
    char *const clamped = requested < end ? end : requested;

    if (reinterpret_cast<uintptr_t>(clamped) > kHeapLimit) {
        ReportHeapExhaustedAndReset();
        // Not reached: NVIC_SystemReset() does not return.
        return reinterpret_cast<void *>(-1);
    }

    s_brk = clamped;
    if (s_brk > s_highWater) {
        s_highWater = s_brk;
    }

    return base;
}

namespace bkshepherd {

uint32_t HeapUsedBytes() {
    const char *brk = (s_brk == nullptr) ? end : s_brk;
    return static_cast<uint32_t>(brk - end);
}

uint32_t HeapTotalBytes() { return static_cast<uint32_t>(kHeapLimit - reinterpret_cast<uintptr_t>(end)); }

} // namespace bkshepherd
