#pragma once
#ifndef AUDIO_GUARD_H
#define AUDIO_GUARD_H

// Header-only audio safety helpers used by the audio callback.
// Deliberately free of Daisy/STM32 includes so tests/ can build it on a host machine.
//
// The firmware is built with -Ofast, which implies -ffinite-math-only. Under that flag the
// compiler may assume no NaN or infinity exists, so std::isnan folds to false and
// std::isfinite folds to true, and a guard written with them compiles to nothing. These
// helpers therefore test the IEEE-754 bit pattern directly, behind an optimization barrier.

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace bkshepherd {
namespace audio_guard {

/** Raw IEEE-754 single-precision bits of x. */
inline uint32_t FloatBits(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    // Empty asm (no instructions) that hides u's origin from the optimizer. Without it,
    // clang under fast-math recognizes the exponent-mask test below as a floating-point
    // class check and folds it using its no-NaN/no-infinity assumption.
    __asm__("" : "+r"(u));
    return u;
}

/** True for any finite value (exponent field not all ones). Safe under -ffinite-math-only. */
inline bool IsFiniteBits(float x) { return (FloatBits(x) & 0x7F800000u) != 0x7F800000u; }

/** True only for NaN (exponent all ones, non-zero mantissa). Safe under -ffinite-math-only. */
inline bool IsNanBits(float x) {
    const uint32_t u = FloatBits(x);
    return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0;
}

/** Limit an input sample to [-1, 1]. NaN becomes 0. Infinity becomes +/-1. */
inline float ClampInput(float x) {
    if (IsNanBits(x)) {
        return 0.0f;
    }
    if (!IsFiniteBits(x)) {
        // Infinity: pick the rail from the sign bit rather than relying on a comparison
        // the optimizer is allowed to assume never sees infinity.
        return (FloatBits(x) & 0x80000000u) ? -1.0f : 1.0f;
    }
    return std::clamp(x, -1.0f, 1.0f);
}

/** Replace non-finite (NaN or infinite) samples with silence.
 *  Returns true if either sample had to be replaced, so the caller can react
 *  (mute briefly, reset the effect) rather than letting garbage reach the DAC. */
inline bool SanitizePair(float &left, float &right) {
    bool replaced = false;
    if (!IsFiniteBits(left)) {
        left = 0.0f;
        replaced = true;
    }
    if (!IsFiniteBits(right)) {
        right = 0.0f;
        replaced = true;
    }
    return replaced;
}

} // namespace audio_guard
} // namespace bkshepherd

#endif
