#pragma once
#ifndef AUDIO_GUARD_H
#define AUDIO_GUARD_H

// Header-only audio safety helpers used by the audio callback.
// Deliberately free of Daisy/STM32 includes so tests/ can build it on a host machine.

#include <algorithm>
#include <cmath>

namespace bkshepherd {
namespace audio_guard {

/** Limit an input sample to [-1, 1]. NaN becomes 0. Infinity becomes +/-1. */
inline float ClampInput(float x) {
    if (std::isnan(x)) {
        return 0.0f;
    }
    return std::clamp(x, -1.0f, 1.0f);
}

/** Replace non-finite (NaN or infinite) samples with silence.
 *  Returns true if either sample had to be replaced, so the caller can react
 *  (mute briefly, reset the effect) rather than letting garbage reach the DAC. */
inline bool SanitizePair(float &left, float &right) {
    bool replaced = false;
    if (!std::isfinite(left)) {
        left = 0.0f;
        replaced = true;
    }
    if (!std::isfinite(right)) {
        right = 0.0f;
        replaced = true;
    }
    return replaced;
}

} // namespace audio_guard
} // namespace bkshepherd

#endif
