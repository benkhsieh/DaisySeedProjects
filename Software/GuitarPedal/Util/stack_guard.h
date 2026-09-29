#pragma once
#ifndef STACK_GUARD_H
#define STACK_GUARD_H

#include <cstdint>

// Stack high-water measurement. The stack is the only thing in the gap between the end
// of the DTCM statics and _estack in DTCM; overflowing it corrupts newlib's allocator
// state, which sits directly below the stack floor. Paint the free part at boot, then
// look for the highest unpainted byte later.

// Declared as incomplete array types (not single `char` objects) so -Ofast cannot reason
// about pointer arithmetic as walking off the end of a 1-byte object; these are linker
// symbols marking addresses, not real objects.
extern "C" char _edtcmram_bss[]; // end of DTCM statics, provided by the linker script
extern "C" char _estack[];

namespace bkshepherd {

constexpr uint8_t kStackPaint = 0xA5;

inline uint32_t StackTotalBytes() {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_estack) - reinterpret_cast<uintptr_t>(_edtcmram_bss));
}

/** Call first thing in main(). Paints from the stack floor up to 64 bytes below the
 *  current stack pointer so the frame we are running in is left alone. */
inline void StackPaint() {
    uint8_t *floor = reinterpret_cast<uint8_t *>(_edtcmram_bss);
    uint8_t *sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    uint8_t *top = sp - 64;
    for (uint8_t *p = floor; p < top; ++p) {
        *p = kStackPaint;
    }
}

/** Minimum free stack observed since StackPaint(): number of still-painted bytes above
 *  the floor. Stops at the first unpainted byte, so a lucky 0xA5 in real data only
 *  over-reports free space by a few bytes. */
inline uint32_t StackFreeBytes() {
    const uint8_t *floor = reinterpret_cast<const uint8_t *>(_edtcmram_bss);
    const uint8_t *top = reinterpret_cast<const uint8_t *>(_estack);
    uint32_t n = 0;
    for (const uint8_t *p = floor; p < top && *p == kStackPaint; ++p) {
        ++n;
    }
    return n;
}

} // namespace bkshepherd

#endif
