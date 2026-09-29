#pragma once
#ifndef TAPE_SATURATOR_H
#define TAPE_SATURATOR_H

// Soft saturation for a tape echo's feedback loop. Header-only, no Daisy includes, so
// tests/ can build it on a host machine.

namespace bkshepherd {
namespace tape_saturator_detail {

// Cheap odd rational tanh approximation, accurate to about 1% on [-3, 3]. It reaches
// exactly 1 with zero slope at |x| = 3, so clamping the input there keeps it monotonic.
inline float TanhApprox(float x) {
    if (x > 3.0f) {
        x = 3.0f;
    }
    if (x < -3.0f) {
        x = -3.0f;
    }
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

} // namespace tape_saturator_detail

/** Saturate x with the given drive (>= 1). Odd, monotonic, with unity small-signal gain at
 *  every drive; the output ceiling is 1 / drive, so more drive compresses sooner and lower.
 *  At drive 1 a full-scale input of 1.0 maps to about 0.78. */
inline float TapeSaturate(float x, float drive) {
    using tape_saturator_detail::TanhApprox;
    if (drive < 1.0f) {
        drive = 1.0f;
    }
    return TanhApprox(drive * x) / drive;
}

} // namespace bkshepherd

#endif
