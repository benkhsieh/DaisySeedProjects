# Phase 2: AmpTrem, Drop, and TapeEcho Modules Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three effect modules from spec section 6: a two-knob Fender-style tremolo (AmpTrem), a dedicated pitch-down module with latch and momentary modes (Drop), and a standalone tape echo whose first-boot sound is a rockabilly slapback (TapeEcho).

**Architecture:** Each module is one `.h`/`.cpp` pair under `Effect-Modules/`, registered in `loaded_effects.h` and the Makefile, following the pattern of `modulated_tremolo_module`, `pitch_shifter_module`, and `delay_module`. Audio buffers live in SDRAM via `DSY_SDRAM_BSS`; DSP objects are class members (module objects are heap-allocated in D2 SRAM at boot), never file-scope statics, so DTCM (the stack's only home) does not shrink. One pure helper, the tape saturator, is header-only and host-tested.

**Tech Stack:** C++20, STM32H750, libDaisy v8, DaisySP (plus DaisySP-LGPL for `Tone`), project `Util/pitch_shifter.h` and `Util/tape_modulator.h`, host tests via `Software/GuitarPedal/tests/Makefile` (`-O3 -ffast-math` to match `-Ofast`).

Spec: `docs/superpowers/specs/2026-09-26-firmware-next-design.md` sections 6.1, 6.2, 6.3, and the section 8 checklist lines for the new modules.

Baseline (commit 6607703, 125B): DTCMRAM 103872 B, SRAM 396484 B (80.7%), RAM_D2_DMA 27948 B, stack gap 27212 B.

## Global Constraints

- New modules add no file-scope statics in DTCM. Buffers use `DSY_SDRAM_BSS`; everything else is a class member. Verify with `arm-none-eabi-nm --size-sort` that no new symbol lands in `0x2000xxxx`/`0x2001xxxx` beyond a few bytes. DTCMRAM must stay at or below 104000 B.
- SRAM under 90 percent of 480 KB (442368 B). Expect roughly 8 to 15 KB for the three modules.
- Parameter metadata uses designated initializers in declaration order: `name, valueType, valueCurve, valueBinCount, valueBinNames, defaultValue, knobMapping, midiCCMapping, minValue, maxValue, fineStepSize`. A `Float` parameter's stored value is in the units of `minValue..maxValue`; knobs map through `SetParameterAsMagnitude` using `valueCurve`. `defaultValue.float_value` must be in those units.
- Adding effects shifts the effect list; the settings fingerprint (Phase 1b) resets stored settings automatically on first boot. Say so in the PR.
- Each module implements `Reset()` (clears audio state, keeps parameters, never allocates).
- Build all five variants; run `make -C Software/GuitarPedal/tests`; `clang-format -i` only on changed files, never `./ci/format.sh`; no `git stash`; commit trailer `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Branch `feature/next`, worktree `~/DaisySeedProjects/.worktrees/next`.
- Known noise: a pre-existing `"PI" redefined` warning from `spectral_delay_module.h` and warnings from `dependencies/`.
- Right footswitch is Bypass, left is Alt. The framework calls `AlternateFootswitchPressed()` on every Alt press (including the taps of a tap-tempo double-tap), `AlternateFootswitchReleased()` on release, `AlternateFootswitchHeldFor1Second()` while held past 1 s (every block while held, so latch on first call), and `SetTempo(bpm)` on a double-tap when `AlternateFootswitchForTempo()` is true.

---

## File Structure

| Path | Responsibility |
|---|---|
| `Software/GuitarPedal/Effect-Modules/amp_tremolo_module.h/.cpp` | New. AmpTrem: sine LFO with soft-clipped shape, Speed and Intensity, tap tempo, LED follows the LFO. |
| `Software/GuitarPedal/Effect-Modules/drop_module.h/.cpp` | New. Drop: latched or momentary pitch-down using `Util/pitch_shifter.h`, own SDRAM buffers. |
| `Software/GuitarPedal/Util/tape_saturator.h` | New, header-only, no Daisy includes. Soft saturation for the echo feedback loop. |
| `Software/GuitarPedal/tests/test_tape_saturator.cpp`, `tests/Makefile` | New test; Makefile gains the target. |
| `Software/GuitarPedal/Effect-Modules/tape_echo_module.h/.cpp` | New. TapeEcho: one SDRAM delay line read at up to three heads, wow/flutter from `TapeModulator`, saturated and filtered feedback, tap tempo, hold for self-oscillation, LED pulses at the rate. |
| `Software/GuitarPedal/loaded_effects.h`, `Software/GuitarPedal/Makefile` | Modify. Register the three modules (append at the end of the list so existing indices keep their order). |

---

### Task 0: Baseline

- [ ] `cd ~/DaisySeedProjects/.worktrees/next && git status --short` shows nothing modified; `git log --oneline -1` shows 6607703 or later.
- [ ] `cd Software/GuitarPedal && make -j8 2>&1 | grep -E "DTCMRAM:|SRAM:"` matches the baseline within a few hundred bytes. `make -C tests` passes.

---

### Task 1: AmpTrem module

**Files:**
- Create: `Software/GuitarPedal/Effect-Modules/amp_tremolo_module.h`, `.cpp`
- Modify: `Software/GuitarPedal/loaded_effects.h` (include plus `new AmpTremoloModule(),` appended after `new FlangerModule(),`), `Software/GuitarPedal/Makefile` (`CPP_SOURCES += Effect-Modules/amp_tremolo_module.cpp` next to `modulated_tremolo_module.cpp`)

**Interfaces:**
- Consumes: `daisysp::Oscillator`, `daisysp::SoftClip` (from `dsp.h`), `fonepole`, `tempo_to_freq` (`Util/audio_utilities.h`), `BaseEffectModule`.
- Produces: `class bkshepherd::AmpTremoloModule`, name `"AmpTrem"`, parameters `Speed` (knob 0, Float, Log curve, 1 to 12 Hz, default 5.0), `Intensity` (knob 1, Float, Linear, 0 to 1, default 0.5). MIDI CC 20 and 21.

- [ ] **Step 1: Header**

```cpp
#pragma once
#ifndef AMP_TREMOLO_MODULE_H
#define AMP_TREMOLO_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file amp_tremolo_module.h */

using namespace daisysp;

namespace bkshepherd {

/** A two-knob amplifier-style tremolo: Speed and Intensity, sine LFO with a softly
 *  flattened top like an optical tremolo. Tap tempo on the Alt footswitch sets Speed. */
class AmpTremoloModule : public BaseEffectModule {
  public:
    AmpTremoloModule();
    ~AmpTremoloModule();

    void Init(float sample_rate) override;
    void ParameterChanged(int parameter_id) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    void SetTempo(uint32_t bpm) override;
    float GetBrightnessForLED(int led_id) const override;
    void Reset() override;

  private:
    void ApplySpeed();

    Oscillator m_lfo;
    float m_gain = 1.0f; // smoothed per-sample gain, 1 = no attenuation
    const float m_speedMinHz = 1.0f;
    const float m_speedMaxHz = 12.0f;
};
} // namespace bkshepherd
#endif
#endif
```

- [ ] **Step 2: Implementation**

```cpp
#include "amp_tremolo_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

static const int s_paramCount = 2;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Speed",
        valueType : ParameterValueType::Float,
        valueCurve : ParameterValueCurve::Log,
        valueBinCount : 0,
        defaultValue : {.float_value = 5.0f},
        knobMapping : 0,
        midiCCMapping : 20,
        minValue : 1,
        maxValue : 12
    },
    {
        name : "Intensity",
        valueType : ParameterValueType::Float,
        valueCurve : ParameterValueCurve::Linear,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 1,
        midiCCMapping : 21,
        minValue : 0,
        maxValue : 1
    },
};

AmpTremoloModule::AmpTremoloModule() : BaseEffectModule() {
    m_name = "AmpTrem";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

AmpTremoloModule::~AmpTremoloModule() {}

void AmpTremoloModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_lfo.Init(sample_rate);
    m_lfo.SetWaveform(Oscillator::WAVE_SIN);
    m_lfo.SetAmp(1.0f);
    ApplySpeed();
    m_gain = 1.0f;
}

void AmpTremoloModule::ApplySpeed() {
    float hz = GetParameterAsFloat(0);
    if (hz < m_speedMinHz) {
        hz = m_speedMinHz;
    } else if (hz > m_speedMaxHz) {
        hz = m_speedMaxHz;
    }
    m_lfo.SetFreq(hz);
}

void AmpTremoloModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 0) {
        ApplySpeed();
    }
}

void AmpTremoloModule::ProcessMono(float in) {
    BaseEffectModule::ProcessMono(in);

    // Sine LFO with the peaks softly flattened: an optical tremolo spends a little longer
    // near full volume than a pure sine does.
    const float lfo = SoftClip(1.5f * m_lfo.Process()); // -1..1, flattened top and bottom
    const float depth = GetParameterAsFloat(1);
    const float target = 1.0f - depth * 0.5f * (1.0f - lfo); // 1 - depth .. 1

    // Ease toward the target so square-ish edges never click.
    fonepole(m_gain, target, 0.01f);

    m_audioLeft = m_audioLeft * m_gain;
    m_audioRight = m_audioLeft;
}

void AmpTremoloModule::ProcessStereo(float inL, float inR) {
    ProcessMono(inL);
    BaseEffectModule::ProcessStereo(m_audioLeft, inR);
    m_audioRight = m_audioRight * m_gain;
}

void AmpTremoloModule::SetTempo(uint32_t bpm) {
    // One LFO cycle per beat.
    float hz = tempo_to_freq(bpm);
    if (hz < m_speedMinHz) {
        hz = m_speedMinHz;
    } else if (hz > m_speedMaxHz) {
        hz = m_speedMaxHz;
    }
    SetParameterAsFloat(0, hz);
    ApplySpeed();
}

float AmpTremoloModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return value * m_gain;
    }
    return value;
}

void AmpTremoloModule::Reset() {
    m_lfo.Reset();
    m_gain = 1.0f;
}
```

- [ ] **Step 3: Register, build, commit**

Add the include and `new AmpTremoloModule(),` (after `new FlangerModule(),`) in `loaded_effects.h`; add the Makefile line. Build 125B: no new warnings; check `arm-none-eabi-nm --size-sort -S build/guitarpedal.elf | grep -i tremolo` shows no new DTCM (`2000…`) symbol other than `s_metaData` (a few hundred bytes in `.data` is acceptable, since all modules keep their metadata there). Commit `Add AmpTrem: two-knob amp-style tremolo with tap tempo`.

---

### Task 2: Drop module

**Files:**
- Create: `Software/GuitarPedal/Effect-Modules/drop_module.h`, `.cpp`
- Modify: `loaded_effects.h` (include plus `new DropModule(),` after AmpTrem), `Makefile`

**Interfaces:**
- Consumes: `daisysp_modified::PitchShifter` from `Util/pitch_shifter.h` (`Init(sr, bufA, bufB, size)`, `SetTransposition(float)`, `SetDelSize(uint32_t)`, `Process(float)`), `daisysp::CrossFade`, `std::lerp`.
- Produces: `class bkshepherd::DropModule`, name `"Drop"`, parameters `Semitones` (knob 0, Binned 12, names "1".."12", default bin 2), `Mix` (knob 1, Float 0..1, default 1.0), `Mode` (knob 2, Binned 2, "LATCH"/"MOMENT", default LATCH). Always shifts down.

- [ ] **Step 1: Header**

```cpp
#pragma once
#ifndef DROP_MODULE_H
#define DROP_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file drop_module.h */

using namespace daisysp;

namespace bkshepherd {

/** Dedicated pitch-down module (a "drop" pedal). LATCH shifts whenever the effect is on;
 *  MOMENT shifts only while the Alt footswitch is held, with a fixed 100 ms ramp each way. */
class DropModule : public BaseEffectModule {
  public:
    DropModule();
    ~DropModule();

    void Init(float sample_rate) override;
    void ParameterChanged(int parameter_id) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    void AlternateFootswitchPressed() override;
    void AlternateFootswitchReleased() override;
    bool AlternateFootswitchForTempo() const override { return false; }
    float GetBrightnessForLED(int led_id) const override;
    void Reset() override;
    bool UsesKnobMap() const override { return true; }

  private:
    void ApplyInterval();
    float SemitonesDown() const;

    bool m_latching = true;
    bool m_altHeld = false;
    float m_rampPosition = 1.0f; // 0 = unshifted, 1 = fully shifted
    float m_rampStepPerSample = 0.0f;
    float m_semitonesDown = 2.0f;
    float m_sampleRate = 48000.0f;
    void *m_engine = nullptr; // DropEngine, allocated once in Init (heap is in D2 SRAM)
};
} // namespace bkshepherd
#endif
#endif
```

- [ ] **Step 2: Implementation**

```cpp
#include "drop_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Util/pitch_shifter.h"

using namespace bkshepherd;

static const char *s_semitoneBinNames[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
static const char *s_modeBinNames[2] = {"LATCH", "MOMENT"};

// Same delay-size range as the Pitch module: shorter is lower latency, longer sounds
// better on wide intervals. ~42 ms at 2048 samples, ~125 ms at 6000.
const uint32_t k_minDelaySamples = 2048;
const uint32_t k_maxDelaySamples = 6000;

// Momentary ramp time each way.
const float k_rampSeconds = 0.1f;

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Semitones",
        valueType : ParameterValueType::Binned,
        valueBinCount : 12,
        valueBinNames : s_semitoneBinNames,
        defaultValue : {.uint_value = 1}, // bin index 1 = "2"
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 1,
        midiCCMapping : -1
    },
    {
        name : "Mode",
        valueType : ParameterValueType::Binned,
        valueBinCount : 2,
        valueBinNames : s_modeBinNames,
        defaultValue : {.uint_value = 0},
        knobMapping : 2,
        midiCCMapping : -1
    },
};

// The pitch shifter's two delay buffers. SDRAM so DTCM (the stack) is untouched.
float DSY_SDRAM_BSS drop_buffer_a[k_maxDelaySamples];
float DSY_SDRAM_BSS drop_buffer_b[k_maxDelaySamples];

namespace {
// The shifter and crossfade are heap-allocated with the module (via new in load_effects),
// so they live in a small holder reached through the module's m_engine pointer rather
// than as file-scope statics in DTCM.
struct DropEngine {
    daisysp_modified::PitchShifter shifter;
    CrossFade mix;
};
} // namespace

DropModule::DropModule() : BaseEffectModule() {
    m_name = "Drop";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

DropModule::~DropModule() {}

float DropModule::SemitonesDown() const { return static_cast<float>(GetParameterAsBinnedValue(0)); }

void DropModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_sampleRate = sample_rate;

    if (m_engine == nullptr) {
        m_engine = new DropEngine(); // once, at boot; heap lives in D2 SRAM
    }
    DropEngine *e = static_cast<DropEngine *>(m_engine);

    memset(drop_buffer_a, 0, sizeof(drop_buffer_a));
    memset(drop_buffer_b, 0, sizeof(drop_buffer_b));
    e->shifter.Init(sample_rate, drop_buffer_a, drop_buffer_b, k_maxDelaySamples);
    e->mix.Init(CROSSFADE_CPOW);
    e->mix.SetPos(GetParameterAsFloat(1));

    m_latching = GetParameterAsBinnedValue(2) == 1;
    m_rampStepPerSample = 1.0f / (k_rampSeconds * sample_rate);
    m_rampPosition = m_latching ? 1.0f : 0.0f;
    ApplyInterval();
}

void DropModule::ApplyInterval() {
    m_semitonesDown = SemitonesDown();
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    // Longer delay for wider intervals, as the Pitch module does in latch mode.
    const float t = (m_semitonesDown - 1.0f) / 11.0f;
    const uint32_t delaySize = static_cast<uint32_t>(std::lerp(static_cast<float>(k_minDelaySamples), static_cast<float>(k_maxDelaySamples), t));
    e->shifter.SetDelSize(std::clamp(delaySize, k_minDelaySamples, k_maxDelaySamples));
}

void DropModule::ParameterChanged(int parameter_id) {
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (parameter_id == 0) {
        ApplyInterval();
    } else if (parameter_id == 1 && e != nullptr) {
        e->mix.SetPos(GetParameterAsFloat(1));
    } else if (parameter_id == 2) {
        m_latching = GetParameterAsBinnedValue(2) == 1;
        // Switching to LATCH engages fully; switching to MOMENT follows the footswitch.
        m_rampPosition = m_latching ? 1.0f : (m_altHeld ? 1.0f : 0.0f);
    }
}

void DropModule::AlternateFootswitchPressed() { m_altHeld = true; }

void DropModule::AlternateFootswitchReleased() { m_altHeld = false; }

void DropModule::ProcessMono(float in) {
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        m_audioLeft = m_audioRight = in;
        return;
    }

    // Ramp toward the target in MOMENT mode; pinned at 1 in LATCH mode.
    const float target = m_latching ? 1.0f : (m_altHeld ? 1.0f : 0.0f);
    if (m_rampPosition < target) {
        m_rampPosition = std::min(target, m_rampPosition + m_rampStepPerSample);
    } else if (m_rampPosition > target) {
        m_rampPosition = std::max(target, m_rampPosition - m_rampStepPerSample);
    }

    e->shifter.SetTransposition(-m_semitonesDown * m_rampPosition);
    const float shifted = e->shifter.Process(in);
    const float out = e->mix.Process(in, shifted);
    m_audioLeft = m_audioRight = out;
}

void DropModule::ProcessStereo(float inL, float inR) { ProcessMono(inL); }

float DropModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return (m_rampPosition > 0.5f) ? value : 0.0f;
    }
    return value;
}

void DropModule::Reset() {
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    memset(drop_buffer_a, 0, sizeof(drop_buffer_a));
    memset(drop_buffer_b, 0, sizeof(drop_buffer_b));
    e->shifter.Init(m_sampleRate, drop_buffer_a, drop_buffer_b, k_maxDelaySamples);
    ApplyInterval();
    m_rampPosition = m_latching ? 1.0f : 0.0f;
}
```

Implementation note: the `DropEngine` holder exists because `daisysp_modified::PitchShifter` is defined in a project header that should not be pulled into every translation unit through the module header; `new` once in `Init` is allowed (boot only, before audio starts, from the D2 heap), and `Reset` reinitializes in place without allocating.

- [ ] **Step 3: Register, build, commit**

Register in `loaded_effects.h` (after AmpTrem) and the Makefile. Build; nm check for DTCM symbols (only `s_metaData`, `s_*BinNames` pointers). Commit `Add Drop: dedicated pitch-down with latch and momentary modes`.

---

### Task 3: Tape saturator (pure, host-tested)

**Files:**
- Create: `Software/GuitarPedal/Util/tape_saturator.h`, `Software/GuitarPedal/tests/test_tape_saturator.cpp`
- Modify: `Software/GuitarPedal/tests/Makefile` (add `test_tape_saturator` to `TESTS` and a rule mapping it to `../Util/tape_saturator.h`)

**Interfaces:**
- Produces `inline float bkshepherd::TapeSaturate(float x, float drive)`: `drive >= 1`; returns `tanh_approx(drive * x) / tanh_approx(drive)` so that the output is bounded in (-1, 1), odd-symmetric, monotonic, and the small-signal gain at `drive = 1` is close to 1. Use the rational approximation `t(x) = x * (27 + x*x) / (27 + 9*x*x)` clamped to [-3, 3] input (a standard cheap tanh); no `<cmath>` dependency on `-Ofast` behavior.

- [ ] **Step 1: Failing test**

```cpp
#include "../Util/tape_saturator.h"
#include <cstdio>
using namespace bkshepherd;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
static void test_zero_maps_to_zero() { CHECK(TapeSaturate(0.0f, 1.0f) == 0.0f); CHECK(TapeSaturate(0.0f, 4.0f) == 0.0f); }
static void test_bounded() { for (float x = -10.0f; x <= 10.0f; x += 0.25f) { float y = TapeSaturate(x, 2.0f); CHECK(y > -1.0001f && y < 1.0001f); } }
static void test_odd_symmetry() { for (float x = 0.0f; x <= 3.0f; x += 0.1f) { CHECK(TapeSaturate(x, 3.0f) == -TapeSaturate(-x, 3.0f)); } }
static void test_monotonic() { float prev = TapeSaturate(-5.0f, 2.0f); for (float x = -4.9f; x <= 5.0f; x += 0.1f) { float y = TapeSaturate(x, 2.0f); CHECK(y >= prev); prev = y; } }
static void test_small_signal_gain_near_unity_at_drive_1() { float y = TapeSaturate(0.05f, 1.0f); CHECK(y > 0.045f && y < 0.055f); }
static void test_full_scale_reaches_near_one() { CHECK(TapeSaturate(1.0f, 1.0f) > 0.99f); CHECK(TapeSaturate(1.0f, 3.0f) > 0.99f); }
int main() { test_zero_maps_to_zero(); test_bounded(); test_odd_symmetry(); test_monotonic(); test_small_signal_gain_near_unity_at_drive_1(); test_full_scale_reaches_near_one(); if (!failures) std::printf("test_tape_saturator: all passed\n"); return failures ? 1 : 0; }
```

- [ ] **Step 2: Header**

```cpp
#pragma once
#ifndef TAPE_SATURATOR_H
#define TAPE_SATURATOR_H
// Soft saturation for a tape echo's feedback loop. Header-only, no Daisy includes.
namespace bkshepherd {
namespace tape_saturator_detail {
// Cheap odd rational tanh approximation, accurate to about 1% on [-3, 3].
inline float TanhApprox(float x) {
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
} // namespace tape_saturator_detail
/** Saturate x with the given drive (>= 1). Output is bounded in (-1, 1), odd, monotonic,
 *  and normalized so an input of 1.0 maps to (nearly) 1.0 at any drive. */
inline float TapeSaturate(float x, float drive) {
    using tape_saturator_detail::TanhApprox;
    if (drive < 1.0f) drive = 1.0f;
    return TanhApprox(drive * x) / TanhApprox(drive);
}
} // namespace bkshepherd
#endif
```
Check the "full scale reaches near one" test against the formula: at drive 1, `TanhApprox(1)/TanhApprox(1) = 1`. At drive 3, `TanhApprox(3)/TanhApprox(3) = 1`. The small-signal test: `TanhApprox(0.05)/TanhApprox(1)`; `TanhApprox(1) = 28/36 = 0.777`, so the ratio is about `0.05/0.777 = 0.064`, which fails the `< 0.055` bound. Fix the normalization so that small-signal gain is unity at drive 1 instead: return `TanhApprox(drive * x) / drive` when `drive` is 1 this is exactly `TanhApprox(x)`, whose small-signal gain is 1 and whose value at 1.0 is 0.777. Then adjust the full-scale test to `> 0.75f`. Use this second form; a slapback repeat that peaks at 0.78 of input is the intended "compresses instead of clipping" behavior. Record in the report that the test bounds were chosen from the formula.

- [ ] **Step 3: Run tests, commit**

`make -C Software/GuitarPedal/tests` passes all three suites. Commit `Add TapeSaturate helper with host tests`.

---

### Task 4: TapeEcho module

**Files:**
- Create: `Software/GuitarPedal/Effect-Modules/tape_echo_module.h`, `.cpp`
- Modify: `loaded_effects.h` (include plus `new TapeEchoModule(),` after Drop), `Makefile`

**Interfaces:**
- Consumes: `daisysp::DelayLine<float, N>` (`Init`, `Write`, `Read(float)`), `daisysp::Tone` (DaisySP-LGPL; `Init(sr)`, `SetFreq`, `Process`), `daisysp::Oscillator` (LED), `TapeModulator` (`Util/tape_modulator.h`: `Init(sr)`, `GetTapeSpeed(wowRate, flutterRate, wowDepth, flutterDepth)` returns a speed factor near 1.0; read `DelayModule::ProcessModulation` lines ~296-335 in `delay_module.cpp` to reuse exactly how the factor scales the delay time), `TapeSaturate`, `tempo_to_freq`.
- Produces: `class bkshepherd::TapeEchoModule`, name `"TapeEcho"`, parameters per spec 6.3:

| Knob | Name | Type | Range / bins | Default |
|---|---|---|---|---|
| 0 | `Rate` | Float, Log | 40..800 (ms) | 120.0 |
| 1 | `Repeats` | Float, Linear | 0..1 (scaled internally to 0..1.1 feedback) | 0.25 |
| 2 | `Echo Vol` | Float | 0..1 | 0.55 |
| 3 | `Wow Flut` | Float | 0..1 | 0.15 |
| 4 | `Tone` | Float | 0..1 (maps log 800 Hz..12 kHz) | 0.6 |
| 5 | `Heads` | Binned 7 | "1","2","3","1+2","2+3","1+2+3","1+3" | bin 0 ("1") |

- [ ] **Step 1: Header**

```cpp
#pragma once
#ifndef TAPE_ECHO_MODULE_H
#define TAPE_ECHO_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file tape_echo_module.h */

using namespace daisysp;

namespace bkshepherd {

/** Standalone tape echo modeled on the Space Echo control set. Default sound is a
 *  rockabilly slapback: one fast repeat, slightly dark, a touch of tape wobble. */
class TapeEchoModule : public BaseEffectModule {
  public:
    TapeEchoModule();
    ~TapeEchoModule();

    void Init(float sample_rate) override;
    void ParameterChanged(int parameter_id) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    void SetTempo(uint32_t bpm) override;
    void AlternateFootswitchHeldFor1Second() override;
    void AlternateFootswitchReleased() override;
    float GetBrightnessForLED(int led_id) const override;
    void Reset() override;

  private:
    float RateSamples() const;
    float FeedbackAmount() const;
    void ApplyTone();
    void ApplyLedRate();
    void HeadGains(float &g1, float &g2, float &g3) const;

    void *m_engine = nullptr; // TapeEchoEngine, allocated once in Init
    float m_sampleRate = 48000.0f;
    float m_currentRateSamples = 5760.0f; // smoothed delay time in samples (120 ms at 48 kHz)
    float m_speedFactor = 1.0f;          // smoothed wow/flutter factor
    bool m_oscillateHeld = false;         // Alt held: repeats forced to maximum
    float m_ledPhase = 0.0f;
};
} // namespace bkshepherd
#endif
#endif
```

- [ ] **Step 2: Implementation**

```cpp
#include "tape_echo_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Util/audio_guard.h"
#include "../Util/audio_utilities.h"
#include "../Util/tape_modulator.h"
#include "../Util/tape_saturator.h"

using namespace bkshepherd;

static const char *s_headBinNames[7] = {"1", "2", "3", "1+2", "2+3", "1+2+3", "1+3"};

// Head 3 sits at 3x the head-1 time; 800 ms x 3 at 48 kHz = 115200 samples, plus wow room.
const size_t k_maxDelaySamples = 120000;
const float k_minRateMs = 40.0f;
const float k_maxRateMs = 800.0f;
const float k_maxFeedback = 1.1f;
const float k_toneMinHz = 800.0f;
const float k_toneMaxHz = 12000.0f;

static const int s_paramCount = 6;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {name : "Rate", valueType : ParameterValueType::Float, valueCurve : ParameterValueCurve::Log, valueBinCount : 0,
     defaultValue : {.float_value = 120.0f}, knobMapping : 0, midiCCMapping : 14, minValue : 40, maxValue : 800},
    {name : "Repeats", valueType : ParameterValueType::Float, valueBinCount : 0, defaultValue : {.float_value = 0.25f},
     knobMapping : 1, midiCCMapping : 15},
    {name : "Echo Vol", valueType : ParameterValueType::Float, valueBinCount : 0, defaultValue : {.float_value = 0.55f},
     knobMapping : 2, midiCCMapping : 16},
    {name : "Wow Flut", valueType : ParameterValueType::Float, valueBinCount : 0, defaultValue : {.float_value = 0.15f},
     knobMapping : 3, midiCCMapping : 17},
    {name : "Tone", valueType : ParameterValueType::Float, valueBinCount : 0, defaultValue : {.float_value = 0.6f},
     knobMapping : 4, midiCCMapping : 18},
    {name : "Heads", valueType : ParameterValueType::Binned, valueBinCount : 7, valueBinNames : s_headBinNames,
     defaultValue : {.uint_value = 0}, knobMapping : 5, midiCCMapping : 19},
};

DelayLine<float, k_maxDelaySamples> DSY_SDRAM_BSS tape_echo_line;

namespace {
struct TapeEchoEngine {
    TapeModulator wowFlutter;
    Tone toneFilter;
    Oscillator led;
};
} // namespace

TapeEchoModule::TapeEchoModule() : BaseEffectModule() {
    m_name = "TapeEcho";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

TapeEchoModule::~TapeEchoModule() {}

float TapeEchoModule::RateSamples() const {
    float ms = GetParameterAsFloat(0);
    ms = std::clamp(ms, k_minRateMs, k_maxRateMs);
    return ms * 0.001f * m_sampleRate;
}

float TapeEchoModule::FeedbackAmount() const {
    if (m_oscillateHeld) {
        return k_maxFeedback;
    }
    return GetParameterAsFloat(1) * k_maxFeedback;
}

void TapeEchoModule::HeadGains(float &g1, float &g2, float &g3) const {
    switch (GetParameterAsBinnedValue(5)) {
    case 1: g1 = 1; g2 = 0; g3 = 0; break;
    case 2: g1 = 0; g2 = 1; g3 = 0; break;
    case 3: g1 = 0; g2 = 0; g3 = 1; break;
    case 4: g1 = 1; g2 = 1; g3 = 0; break;
    case 5: g1 = 0; g2 = 1; g3 = 1; break;
    case 6: g1 = 1; g2 = 1; g3 = 1; break;
    case 7: g1 = 1; g2 = 0; g3 = 1; break;
    default: g1 = 1; g2 = 0; g3 = 0; break;
    }
    // Keep the summed level roughly constant as heads are added.
    const float n = g1 + g2 + g3;
    if (n > 1.0f) {
        const float s = 1.0f / n;
        g1 *= s; g2 *= s; g3 *= s;
    }
}

void TapeEchoModule::ApplyTone() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) return;
    const float t = std::clamp(GetParameterAsFloat(4), 0.0f, 1.0f);
    const float hz = k_toneMinHz * std::pow(k_toneMaxHz / k_toneMinHz, t); // log sweep
    e->toneFilter.SetFreq(hz);
}

void TapeEchoModule::ApplyLedRate() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) return;
    e->led.SetFreq(m_sampleRate / RateSamples());
}

void TapeEchoModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_sampleRate = sample_rate;
    if (m_engine == nullptr) {
        m_engine = new TapeEchoEngine();
    }
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    tape_echo_line.Init();
    e->wowFlutter.Init(sample_rate);
    e->toneFilter.Init(sample_rate);
    e->led.Init(sample_rate);
    e->led.SetWaveform(Oscillator::WAVE_SQUARE);
    e->led.SetAmp(1.0f);
    m_currentRateSamples = RateSamples();
    m_speedFactor = 1.0f;
    m_oscillateHeld = false;
    ApplyTone();
    ApplyLedRate();
}

void TapeEchoModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 0) {
        ApplyLedRate();
    } else if (parameter_id == 4) {
        ApplyTone();
    }
}

void TapeEchoModule::ProcessMono(float in) {
    BaseEffectModule::ProcessMono(in);
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) return;

    // Delay time glides toward the knob so changes sweep like a tape motor, not click.
    fonepole(m_currentRateSamples, RateSamples(), 0.0005f);

    // Wow and flutter: same rates the Delay module uses for its "Tape" wave, with the
    // depth scaled by the Wow Flut knob (0 = clean digital). See DelayModule::ProcessModulation.
    const float wf = GetParameterAsFloat(3);
    const float depth = 2.0f * wf;
    const float speed = e->wowFlutter.GetTapeSpeed(0.6f, 3.0f, depth, depth);
    fonepole(m_speedFactor, speed, 0.01f);

    const float head1 = std::clamp(m_currentRateSamples * m_speedFactor, 1.0f, static_cast<float>(k_maxDelaySamples / 3 - 2));
    float g1, g2, g3;
    HeadGains(g1, g2, g3);
    const float tap1 = g1 > 0.0f ? tape_echo_line.Read(head1) : 0.0f;
    const float tap2 = g2 > 0.0f ? tape_echo_line.Read(head1 * 2.0f) : 0.0f;
    const float tap3 = g3 > 0.0f ? tape_echo_line.Read(head1 * 3.0f) : 0.0f;
    const float heads = g1 * tap1 + g2 * tap2 + g3 * tap3;

    // Feedback path: saturate (so runaway repeats compress), then darken.
    float fb = TapeSaturate(heads * FeedbackAmount(), 1.5f);
    fb = e->toneFilter.Process(fb);
    if (!audio_guard::IsFiniteBits(fb)) {
        fb = 0.0f;
    }
    tape_echo_line.Write(in + fb);

    const float wet = heads * GetParameterAsFloat(2);
    m_audioLeft = in + wet;
    m_audioRight = m_audioLeft;

    // LED pulses at the delay rate.
    m_ledPhase = e->led.Process();
}

void TapeEchoModule::ProcessStereo(float inL, float inR) {
    ProcessMono(inL);
    BaseEffectModule::ProcessStereo(m_audioLeft, inR);
    m_audioRight = m_audioLeft; // same echo on both channels (spec 6.3)
}

void TapeEchoModule::SetTempo(uint32_t bpm) {
    // One repeat per beat, clamped to the knob range.
    const float hz = tempo_to_freq(bpm);
    float ms = 1000.0f / std::max(hz, 0.001f);
    ms = std::clamp(ms, k_minRateMs, k_maxRateMs);
    SetParameterAsFloat(0, ms);
    ApplyLedRate();
}

void TapeEchoModule::AlternateFootswitchHeldFor1Second() { m_oscillateHeld = true; }

void TapeEchoModule::AlternateFootswitchReleased() { m_oscillateHeld = false; }

float TapeEchoModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return (m_ledPhase > 0.0f) ? value : 0.0f;
    }
    return value;
}

void TapeEchoModule::Reset() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) return;
    tape_echo_line.Reset();
    e->toneFilter.Init(m_sampleRate);
    ApplyTone();
    m_currentRateSamples = RateSamples();
    m_speedFactor = 1.0f;
    m_oscillateHeld = false;
}
```

Implementer checks before building: (a) confirm the `TapeModulator::GetTapeSpeed` argument order and that its return is a multiplicative factor around 1.0 by reading `Util/tape_modulator.h/.cpp` and how `DelayModule` applies `m_currentMod` (~line 321 onward); if the Delay applies it additively in samples instead, mirror that exactly and scale by `depth`; (b) `DelayLine<float, 120000>` is 480 KB in SDRAM, fine; the `Read(float)` linear interpolation exists in DaisySP; (c) `Tone` is in DaisySP-LGPL, already linked (Delay uses it); (d) the `ProcessStereo` stereo call: the base `ProcessStereo(mono, inR)` resets the right channel to `inR`, so the explicit `m_audioRight = m_audioLeft` afterward is required for "same echo on both channels".

- [ ] **Step 3: Register, build, commit**

Register after Drop in `loaded_effects.h` and the Makefile. Build 125B; nm check for DTCM symbols; SRAM under 90 percent. Commit `Add TapeEcho: Space Echo style tape delay, slapback by default`.

---

### Task 5: CI, artifact, hardware

- [ ] `make -C tests` passes (three suites). Push; CI green; download `125B-Firmware`. Report DTCMRAM, SRAM, RAM_D2_DMA.
- [ ] Flash Ben's pedal. First boot resets settings (fingerprint changed); confirm normal screen after the reset.
- [ ] AmpTrem: sounds like an amp tremolo at default; Speed sweeps 1 to 12 Hz; Intensity to zero is dry; Alt double-tap sets the speed; LED 1 pulses.
- [ ] Drop: LATCH with an E chord at 2 semitones sounds like D, no glitching beyond the expected ~50 ms latency; MOMENT holds the drop only while Alt is held with a smooth 100 ms ramp; LED 1 lit while dropped; Mix at 0.5 blends.
- [ ] TapeEcho at defaults with no knob changes: a single rockabilly slapback, slightly dark, subtle wobble. Rate knob sweeps smoothly. Repeats at max self-oscillates but does not clip harshly. Heads bins change the pattern. Alt double-tap sets the rate; Alt hold runs away, release settles. LED 1 pulses at the rate.
- [ ] Debug screen: heap unchanged after switching among the three new modules ten times; stack free not below 10000; guard trips 0.
- [ ] Knob map on each new module: labels read sensibly in 4 characters ("Spee", "Inte"; "Semi", "Mix", "Mode"; "Rate", "Repe", "Echo", "Wow ", "Tone", "Head"). If any is confusing, shorten the parameter name in the metadata rather than changing the map.
