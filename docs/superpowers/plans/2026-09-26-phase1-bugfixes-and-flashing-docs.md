# Phase 1: Bug Fixes, Crash Guard, and Flashing Guides Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a 125B firmware build that fixes the tuner-gets-no-audio bug and the LEDs-dark-after-effect-switch bug, survives non-finite audio and hard faults without freezing, records what crashed, and comes with step-by-step flashing guides for macOS and Windows.

**Architecture:** All runtime changes live in `Software/GuitarPedal/guitar_pedal.cpp` (the main loop and audio callback) plus three new small headers under `Util/` and one virtual hook on `BaseEffectModule`. Pure logic (audio sanitizing, crash-record validation) is header-only with no Daisy dependencies so it can be unit-tested on the Mac with a tiny host Makefile. Everything else is verified by building for all five variants and by a hardware checklist on Ben's pedal.

**Tech Stack:** C++20 on STM32H750 via libDaisy v8 and DaisySP, `arm-none-eabi-gcc` 10.3, GNU make, `dfu-util` 0.11, Apple clang for host tests, GitHub Actions "Build All" workflow.

Spec: `docs/superpowers/specs/2026-09-26-firmware-next-design.md`, sections 2 and 7 (flashing guides). Sections 3 to 6 and the manual are later plans.

## Global Constraints

- Compiler: `arm-none-eabi-gcc` major version must be 10. The Makefile errors otherwise.
- App type stays `APP_TYPE = BOOT_SRAM`. Firmware is flashed to QSPI at `0x90040000` through the Daisy bootloader.
- Memory after this plan on the 125B variant: DTCMRAM under 95 percent (baseline 91.47 percent, 119892 B of 128 KB), SRAM under 90 percent (baseline 83.12 percent, 408564 B of 480 KB). The only DTCM additions allowed are a handful of scalars. Buffers go in SRAM or SDRAM.
- Every change must build for all five variants: `VARIANT=125B`, `1590B`, `1590B_SMD`, `TERRARIUM`, `FUNBOX`.
- Code style: run `./ci/format.sh` (clang-format) before each commit that touches C++.
- Work happens in the worktree `~/DaisySeedProjects/.worktrees/next` on branch `feature/next`. Never build from `~/DaisySeedProjects` itself; it is a stale 2024 checkout.
- Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Footswitch naming in docs: right footswitch is Bypass, left footswitch is Alt.

---

## File Structure

| Path | Responsibility |
|---|---|
| `Software/GuitarPedal/Util/audio_guard.h` | New. Header-only, no Daisy includes. Clamp input samples, detect and replace non-finite output samples. |
| `Software/GuitarPedal/Util/crash_record.h` | New. Header-only, no Daisy includes. `CrashRecord` struct layout, magic, checksum, validity check. |
| `Software/GuitarPedal/Util/crash_handler.cpp` | New. Defines `HardFault_Handler` (overrides libDaisy's weak default), writes a `CrashRecord` into backup SRAM, then spins until the watchdog reboots. |
| `Software/GuitarPedal/Util/watchdog.h` | New. Thin wrapper over the STM32 HAL IWDG: `WatchdogStart(seconds)`, `WatchdogKick()`. |
| `Software/GuitarPedal/Effect-Modules/base_effect_module.h` / `.cpp` | Modify. Add `virtual void Reset()` (default no-op) so the crash guard can clear an effect's internal state. |
| `Software/GuitarPedal/Effect-Modules/delay_module.h` / `.cpp` | Modify. Override `Reset()` to clear the delay lines. |
| `Software/GuitarPedal/guitar_pedal.cpp` | Modify. `SetActiveEffect` owns enable state and tuner on/off state; audio callback uses the guard; main loop kicks the watchdog and handles guard recovery; startup reports the last crash. |
| `Software/GuitarPedal/Makefile` | Modify. Add `Util/crash_handler.cpp` to `CPP_SOURCES`. |
| `Software/GuitarPedal/tests/Makefile` | New. Host build of the unit tests with Apple clang. |
| `Software/GuitarPedal/tests/test_audio_guard.cpp` | New. Unit tests for `audio_guard.h`. |
| `Software/GuitarPedal/tests/test_crash_record.cpp` | New. Unit tests for `crash_record.h`. |
| `docs/FLASHING-MAC.md` | New. Step-by-step flashing guide for macOS. |
| `docs/FLASHING-WINDOWS.md` | New. Step-by-step flashing guide for Windows. |
| `README.md` | Modify. Link the two guides. |

---

### Task 0: Environment and baseline build

Nothing is committed in this task. It proves the toolchain on this Mac and records the baseline memory numbers that later tasks are compared against.

**Files:**
- None modified.

**Interfaces:**
- Produces: a working `build/guitarpedal.bin` from unmodified `feature/next`, and the baseline memory table.

- [ ] **Step 1: Confirm toolchain versions**

Run:
```bash
arm-none-eabi-gcc -dumpversion; dfu-util --version | head -1; make --version | head -1; c++ --version | head -1
```
Expected: `10.3.1`, `dfu-util 0.11`, GNU Make 3.81 or newer, Apple clang. If the gcc major is not 10, stop and install the 10.3-2021.10 Arm GNU toolchain before continuing.

- [ ] **Step 2: Initialize all submodules**

Run from the worktree root:
```bash
cd ~/DaisySeedProjects/.worktrees/next && git submodule update --init --recursive
```
Expected: `Software/GuitarPedal/dependencies/` contains non-empty `libDaisy`, `DaisySP`, `RTNeural`, `eigen`, `gcem`, `q/q`, `q/infra`. `CloudSeed` is checked in directly, not a submodule. This can take several minutes on first run because `eigen` and `RTNeural` are large.

- [ ] **Step 3: Build the libraries**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/build_libs.sh
```
Expected: ends with `done.` three times (libDaisy, DaisySP, Cloudseed) and exit code 0. Libraries land in `dependencies/libDaisy/build/libdaisy.a`, `dependencies/DaisySP/build/libdaisysp.a`, `dependencies/CloudSeed/build/libcloudseed.a`.

- [ ] **Step 4: Build the 125B firmware and record memory usage**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make clean && make -j8 2>&1 | tail -12
```
Expected: `build/guitarpedal.bin` exists and the tail shows a table like:
```
Memory region         Used Size  Region Size  %age Used
           FLASH:          0 GB       128 KB      0.00%
         DTCMRAM:      119892 B       128 KB     91.47%
            SRAM:      408564 B       480 KB     83.12%
      RAM_D2_DMA:       16960 B        32 KB     51.76%
          RAM_D2:          0 GB       256 KB      0.00%
```
Write the DTCMRAM and SRAM numbers into a scratch note; Task 9 compares against them. (Verified on Ben's Mac on 2026-09-26: DTCMRAM 119892 B, SRAM 408528 B, plus a BACKUP_SRAM line showing 12 B, which is libDaisy's own boot info and confirms the section Task 6 uses is live.)

- [ ] **Step 5: Confirm the other four variants build**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && for v in 1590B 1590B_SMD TERRARIUM FUNBOX; do make clean >/dev/null && make -j8 VARIANT=$v 2>&1 | grep -E "error|SRAM:" ; done
```
Expected: four `SRAM:` lines and no `error` lines.

---

### Task 1: Audio guard header with host unit tests

**Files:**
- Create: `Software/GuitarPedal/Util/audio_guard.h`
- Create: `Software/GuitarPedal/tests/Makefile`
- Create: `Software/GuitarPedal/tests/test_audio_guard.cpp`

**Interfaces:**
- Produces:
  - `namespace bkshepherd::audio_guard`
  - `inline float ClampInput(float x)` returns `x` limited to [-1.0f, 1.0f]; a NaN input returns 0.0f.
  - `inline bool SanitizePair(float &left, float &right)` replaces any non-finite value with 0.0f; returns `true` if either was replaced.
  - Header includes only `<cmath>` and `<algorithm>`. No Daisy headers, so it compiles on the host.

- [ ] **Step 1: Write the failing tests**

Create `Software/GuitarPedal/tests/test_audio_guard.cpp`:
```cpp
// Minimal host tests for Util/audio_guard.h. No framework: each check prints and counts failures.
#include "../Util/audio_guard.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace bkshepherd::audio_guard;

static int failures = 0;
#define CHECK(cond)                                                                                                            \
    do {                                                                                                                       \
        if (!(cond)) {                                                                                                         \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                                                         \
            ++failures;                                                                                                        \
        }                                                                                                                      \
    } while (0)

static void test_clamp_input_passes_normal_values() {
    CHECK(ClampInput(0.0f) == 0.0f);
    CHECK(ClampInput(0.5f) == 0.5f);
    CHECK(ClampInput(-0.5f) == -0.5f);
    CHECK(ClampInput(1.0f) == 1.0f);
    CHECK(ClampInput(-1.0f) == -1.0f);
}

static void test_clamp_input_limits_out_of_range() {
    CHECK(ClampInput(3.7f) == 1.0f);
    CHECK(ClampInput(-42.0f) == -1.0f);
    CHECK(ClampInput(std::numeric_limits<float>::infinity()) == 1.0f);
    CHECK(ClampInput(-std::numeric_limits<float>::infinity()) == -1.0f);
}

static void test_clamp_input_nan_becomes_zero() { CHECK(ClampInput(std::numeric_limits<float>::quiet_NaN()) == 0.0f); }

static void test_sanitize_pair_leaves_finite_untouched() {
    float l = 0.25f, r = -0.75f;
    CHECK(SanitizePair(l, r) == false);
    CHECK(l == 0.25f);
    CHECK(r == -0.75f);
}

static void test_sanitize_pair_replaces_nan() {
    float l = std::numeric_limits<float>::quiet_NaN(), r = 0.1f;
    CHECK(SanitizePair(l, r) == true);
    CHECK(l == 0.0f);
    CHECK(r == 0.1f);
}

static void test_sanitize_pair_replaces_inf_on_either_side() {
    float l = 0.1f, r = -std::numeric_limits<float>::infinity();
    CHECK(SanitizePair(l, r) == true);
    CHECK(l == 0.1f);
    CHECK(r == 0.0f);

    float l2 = std::numeric_limits<float>::infinity(), r2 = std::numeric_limits<float>::quiet_NaN();
    CHECK(SanitizePair(l2, r2) == true);
    CHECK(l2 == 0.0f);
    CHECK(r2 == 0.0f);
}

int main() {
    test_clamp_input_passes_normal_values();
    test_clamp_input_limits_out_of_range();
    test_clamp_input_nan_becomes_zero();
    test_sanitize_pair_leaves_finite_untouched();
    test_sanitize_pair_replaces_nan();
    test_sanitize_pair_replaces_inf_on_either_side();
    if (failures == 0) {
        std::printf("test_audio_guard: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
```

Create `Software/GuitarPedal/tests/Makefile`:
```make
# Host-side unit tests for header-only utilities. Run with: make -C tests
# These headers must not include any Daisy or STM32 headers.
CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -O1

TESTS := test_audio_guard test_crash_record
BUILD := build

.PHONY: all clean $(TESTS)

all: $(TESTS)

$(TESTS): %: $(BUILD)/%
	./$<

# Map each test to the header it covers so make rebuilds when the header changes.
$(BUILD)/test_audio_guard: test_audio_guard.cpp ../Util/audio_guard.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_crash_record: test_crash_record.cpp ../Util/crash_record.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make -C tests test_audio_guard
```
Expected: compile error, `'../Util/audio_guard.h' file not found`.

- [ ] **Step 3: Write the header**

Create `Software/GuitarPedal/Util/audio_guard.h`:
```cpp
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make -C tests test_audio_guard
```
Expected: `test_audio_guard: all passed`, exit code 0. (`make -C tests` without a target will fail until Task 5 adds `test_crash_record.cpp`; that is expected for now.)

- [ ] **Step 5: Add the tests build directory to .gitignore and commit**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next && grep -q "Software/GuitarPedal/tests/build" .gitignore || printf "\n# host unit test binaries\nSoftware/GuitarPedal/tests/build/\n" >> .gitignore
git add .gitignore Software/GuitarPedal/Util/audio_guard.h Software/GuitarPedal/tests/Makefile Software/GuitarPedal/tests/test_audio_guard.cpp
git commit -m "Add audio_guard helpers with host unit tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: SetActiveEffect owns enable state and tuner state (fixes tuner and LEDs)

Spec sections 2.1 and 2.2. Both bugs have the same root: `SetActiveEffect` swaps the module pointer without touching enable state. This task makes it the single place that does.

**Files:**
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (globals near line 50 to 60; the tuner quick-switch block near lines 213 to 252; `SetActiveEffect` near lines 327 to 348)

**Interfaces:**
- Consumes: `BaseEffectModule::SetEnabled(bool)`, `tunerModuleIndex`, `effectOn`, `activeEffectID`.
- Produces: `void SetActiveEffect(int effectID)` with the guarantees below. Later plans (gestures, groups) call it and rely on them.
  1. The outgoing module gets `SetEnabled(false)`, the incoming gets `SetEnabled(effectOn)`.
  2. Entering the tuner from a non-tuner effect saves `effectOn` into `effectOnBeforeTuner` and forces `effectOn = true`.
  3. Leaving the tuner to a non-tuner effect restores `effectOn = effectOnBeforeTuner`.
  4. Calling it with the already-active ID is a no-op.

- [ ] **Step 1: Replace the quick-switch state variables**

In `guitar_pedal.cpp`, find:
```cpp
// Used to debounce quick switching to/from the tuner
bool ignoreBypassSwitchUntilNextActuation = false;
bool effectActiveBeforeQuickSwitch = false;
```
Replace with:
```cpp
// Used to debounce quick switching to/from the tuner
bool ignoreBypassSwitchUntilNextActuation = false;

// Effect on/off state to restore when leaving the tuner. Owned by SetActiveEffect.
bool effectOnBeforeTuner = true;
```

- [ ] **Step 2: Rewrite SetActiveEffect**

Find the whole existing function:
```cpp
void SetActiveEffect(int effectID) {
    if (effectID >= 0 && effectID < availableEffectsCount) {
        // Store the last used effect
        prevActiveEffectID = activeEffectID;

        // Update the ID cache
        activeEffectID = effectID;

        // Update the Active Effect directly.
        activeEffect = availableEffects[effectID];

        guitarPedalUI.UpdateActiveEffect(effectID);

        // Get a handle to the persitance storage settings
        Settings &settings = storage.GetSettings();

        // Update the persistant storage setting
        settings.globalActiveEffectID = effectID;

        last_effect_change_time = System::GetNow();
    }
}
```
Replace with:
```cpp
void SetActiveEffect(int effectID) {
    if (effectID < 0 || effectID >= availableEffectsCount || effectID == activeEffectID) {
        return;
    }

    const bool leavingTuner = (activeEffectID == tunerModuleIndex);
    const bool enteringTuner = (effectID == tunerModuleIndex);

    // The outgoing module is no longer driven, so it must not report itself as enabled
    // (its LED brightness and, for some modules, its processing depend on this flag).
    if (activeEffect != nullptr) {
        activeEffect->SetEnabled(false);
    }

    // The tuner is only useful when it is processing audio, so it is always forced on.
    // Remember the state we came from so leaving the tuner restores it, whichever route
    // was used to get here (footswitch hold, menu, encoder, MIDI program change).
    if (enteringTuner && !leavingTuner) {
        effectOnBeforeTuner = effectOn;
        effectOn = true;
    } else if (leavingTuner && !enteringTuner) {
        effectOn = effectOnBeforeTuner;
    }

    prevActiveEffectID = activeEffectID;
    activeEffectID = effectID;
    activeEffect = availableEffects[effectID];

    // The incoming module takes over the current on/off state. Without this the LED
    // for a module reached through the menu or encoder stayed dark until bypass was
    // toggled twice.
    activeEffect->SetEnabled(effectOn);

    guitarPedalUI.UpdateActiveEffect(effectID);

    Settings &settings = storage.GetSettings();
    settings.globalActiveEffectID = effectID;

    last_effect_change_time = System::GetNow();
}
```

- [ ] **Step 3: Simplify the quick-switch block to rely on SetActiveEffect**

Find inside `AudioCallback`:
```cpp
            if (hardware.SupportsDisplay() && tunerModuleIndex > 0) {
                // Start the quick switch to the tuner
                if (activeEffectID == tunerModuleIndex) {
                    // Set back the active effect before the quick switch
                    SetActiveEffect(prevActiveEffectID);

                    // Restore the effect state from when we quick switched, this is an
                    // inverse because the act of holding the switch caused the state to
                    // chnage due to the rising edge being detected
                    effectOn = !effectActiveBeforeQuickSwitch;
                    activeEffect->SetEnabled(effectOn);
                } else {
                    // Store if effect is on or not when quick switching
                    effectActiveBeforeQuickSwitch = effectOn;

                    // Switch to tuner and force it to be enabled
                    SetActiveEffect(tunerModuleIndex);
                    effectOn = true;
                    activeEffect->SetEnabled(effectOn);
                }
                ignoreBypassSwitchUntilNextActuation = true;
            } else {
```
Replace with:
```cpp
            if (hardware.SupportsDisplay() && tunerModuleIndex > 0) {
                // The rising edge of this same press already toggled effectOn. Undo that
                // so SetActiveEffect saves and restores the state the player actually had.
                effectOn = !effectOn;

                if (activeEffectID == tunerModuleIndex) {
                    SetActiveEffect(prevActiveEffectID);
                } else {
                    SetActiveEffect(tunerModuleIndex);
                }
                ignoreBypassSwitchUntilNextActuation = true;
            } else {
```
Also in the `else` branch directly below (the no-screen "cycle to the next effect" path), find:
```cpp
                SetActiveEffect(newActiveEffectId);

                effectOn = false;
                activeEffect->SetEnabled(effectOn);

                ignoreBypassSwitchUntilNextActuation = true;
```
Replace with:
```cpp
                // Cycling on a screenless pedal lands on the new effect bypassed.
                effectOn = false;
                SetActiveEffect(newActiveEffectId);

                ignoreBypassSwitchUntilNextActuation = true;
```

- [ ] **Step 4: Make startup go through the same enable path**

In `main()`, find:
```cpp
    // Set the active effect
    activeEffect = availableEffects[settings.globalActiveEffectID];
    activeEffectID = settings.globalActiveEffectID;
    activeEffect->SetEnabled(effectOn);
```
Replace with:
```cpp
    // Set the active effect. activeEffectID starts at 0, so force the assignment for
    // effect 0 explicitly; SetActiveEffect would treat it as a no-op.
    activeEffectID = -1;
    activeEffect = nullptr;
    SetActiveEffect(settings.globalActiveEffectID);
```
Note: `SetActiveEffect` calls `guitarPedalUI.UpdateActiveEffect`, which is guarded by `hardware.SupportsDisplay()` and is safe before `guitarPedalUI.Init()` because it only re-initializes menu pages from the active effect. Confirm by reading `UI/guitar_pedal_ui.cpp` `UpdateActiveEffect`; it calls `InitEffectUiPages`, which allocates from `activeEffect` and does not touch the display.

- [ ] **Step 5: Build for 125B and check for warnings**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/format.sh && make -j8 2>&1 | grep -E "warning|error|SRAM:|DTCMRAM:"
```
Expected: no `warning` or `error` lines. `effectActiveBeforeQuickSwitch` must not appear anywhere (`grep -n effectActiveBeforeQuickSwitch guitar_pedal.cpp` returns nothing).

- [ ] **Step 6: Flash Ben's pedal and run the hardware checks**

Flash (pedal in bootloader: press RESET, then BOOT within 5 seconds):
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make program-dfu
```
Checks, all must pass:
1. Power up. LED 0 (bypass LED) is lit. Turn the encoder to Settings > Effect and pick a different effect. LED 0 stays lit. Pick another. Still lit.
2. Hold Alt and turn the encoder to step effects. LED 0 stays lit each step.
3. Tap Bypass to turn the effect off (LED 0 off). Go to Settings > Effect > Tuner. Play an open E. The screen shows `E` and a centered meter within a second. Leave the tuner via the menu to another effect. LED 0 is off and the effect is bypassed, matching the state before the tuner.
4. With the effect on, hold Bypass for 2 seconds. Tuner appears, note shows when a string is played. Hold Bypass 2 seconds again. Previous effect returns and is on.
5. With the effect off, hold Bypass 2 seconds, tune, hold again. Previous effect returns and is off.
6. If a MIDI controller is handy: send a program change to the tuner index and back. Same behavior as 3.

If check 3 shows no note while checks 4 and 5 do, the fix is working and the remaining issue is input level or the left-only input; record that and continue.

- [ ] **Step 7: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/guitar_pedal.cpp
git commit -m "SetActiveEffect owns enable and tuner state: fix tuner audio and dark LEDs after switching

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Reset hook on effects, with the Delay module clearing its lines

Spec section 2.3, part of mitigation 1. The crash guard (Task 4) needs a way to clear an effect's internal state without calling `Init` again, because `CloudSeedModule::Init` and `SpectralDelayModule::Init` allocate with `new` and would leak.

**Files:**
- Modify: `Software/GuitarPedal/Effect-Modules/base_effect_module.h` (public virtuals near line 259, next to `SetTempo`)
- Modify: `Software/GuitarPedal/Effect-Modules/base_effect_module.cpp` (after `SetTempo` near line 401)
- Modify: `Software/GuitarPedal/Effect-Modules/delay_module.h` (public overrides)
- Modify: `Software/GuitarPedal/Effect-Modules/delay_module.cpp`

**Interfaces:**
- Produces: `virtual void BaseEffectModule::Reset()`; default does nothing. Contract: after `Reset()` the module's audio state (delay lines, filters, feedback) holds no signal, parameters are unchanged, and `Process*` may be called immediately. Never allocates.
- Delay module override resets `delayLineLeft`, `delayLineRight`, `delayLineRevLeft`, `delayLineRevRight`, `delayLineSpread` using their existing `Reset()` methods.

- [ ] **Step 1: Declare the hook on the base class**

In `base_effect_module.h`, directly after:
```cpp
    virtual void SetTempo(uint32_t bpm);
```
add:
```cpp

    /** Clears any internal audio state (delay lines, filters, feedback paths) without
     *  reallocating or touching parameters. Called by the crash guard after the module
     *  produced non-finite output. The default does nothing; modules with feedback
     *  memory should override it. Must be safe to call from the main loop while the
     *  audio callback is running.
     */
    virtual void Reset();
```

- [ ] **Step 2: Define the default**

In `base_effect_module.cpp`, directly after the `SetTempo` definition:
```cpp
void BaseEffectModule::Reset() {
    // Do nothing by default. Modules with delay lines or feedback override this.
}
```

- [ ] **Step 3: Override in the Delay module**

In `delay_module.h`, in the `public:` section next to the other overrides (`void SetTempo(uint32_t bpm) override;` or similar), add:
```cpp
    void Reset() override;
```
In `delay_module.cpp`, directly after the end of `DelayModule::Init`, add:
```cpp
void DelayModule::Reset() {
    // Clear every delay line so a NaN or runaway feedback value cannot recirculate.
    // Parameters, targets, and filters are left alone.
    delayLineLeft.Reset();
    delayLineRight.Reset();
    delayLineRevLeft.Reset();
    delayLineRevRight.Reset();
    delayLineSpread.Reset();
}
```
Confirm those five names match the file-scope `DSY_SDRAM_BSS` globals at the top of `delay_module.cpp` (lines 14 to 18) and that `DelayLineRevOct`, `DelayLineReverse`, and `DelayLine` each expose `void Reset()` (`Effect-Modules/Delays/delayline_revoct.h:28`, `Effect-Modules/Delays/delayline_reverse.h`, and `dependencies/DaisySP/Source/Utility/delayline.h:38`). If `DelayLineReverse` lacks `Reset()`, add one to that header that zeroes its buffer and write pointer, mirroring `delayline_revoct.h:28`.

- [ ] **Step 4: Build all variants**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/format.sh && for v in 125B 1590B 1590B_SMD TERRARIUM FUNBOX; do make clean >/dev/null && make -j8 VARIANT=$v 2>&1 | grep -E "warning|error|SRAM:"; done
```
Expected: five `SRAM:` lines, no `warning` or `error`.

- [ ] **Step 5: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/Effect-Modules/base_effect_module.h Software/GuitarPedal/Effect-Modules/base_effect_module.cpp Software/GuitarPedal/Effect-Modules/delay_module.h Software/GuitarPedal/Effect-Modules/delay_module.cpp Software/GuitarPedal/Effect-Modules/Delays
git commit -m "Add BaseEffectModule::Reset hook; Delay clears its lines

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Wire the audio guard into the callback and main loop

Spec section 2.3, mitigations 1 and 2.

**Files:**
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (includes at top; globals; `AudioCallback` sample loop near lines 300 to 320; main loop)

**Interfaces:**
- Consumes: `audio_guard::ClampInput`, `audio_guard::SanitizePair` (Task 1), `BaseEffectModule::Reset()` (Task 3).
- Produces: globals `volatile bool guardTripped`, `int guardMuteSamplesRemaining`, `uint32_t guardTripCount`. Later plans read `guardTripCount` for the debug display.

- [ ] **Step 1: Include the header and add state**

At the top of `guitar_pedal.cpp`, after:
```cpp
#include "Util/audio_utilities.h"
```
add:
```cpp
#include "Util/audio_guard.h"
```
After the `CpuLoadMeter cpuLoadMeter;` global, add:
```cpp

// Audio guard state. The callback sets guardTripped when the active effect produced a
// non-finite sample; the main loop resets the effect and clears the flag. While
// guardMuteSamplesRemaining > 0 the output is silenced so the recovery is inaudible.
volatile bool guardTripped = false;
int guardMuteSamplesRemaining = 0;
uint32_t guardTripCount = 0;
const float guardMuteTimeInSeconds = 0.02f;
int guardMuteTimeInSamples;
```

- [ ] **Step 2: Clamp the inputs**

In the per-sample loop of `AudioCallback`, find:
```cpp
        // Handle Mono vs Stereo
        inputLeft = in[0][i];
        inputRight = in[1][i];
```
Replace with:
```cpp
        // Handle Mono vs Stereo. Clamp so a hot signal or a codec glitch cannot push an
        // out-of-range value into an effect's feedback path.
        inputLeft = audio_guard::ClampInput(in[0][i]);
        inputRight = audio_guard::ClampInput(in[1][i]);
```

- [ ] **Step 3: Sanitize the effect output and apply the guard mute**

Find:
```cpp
            effectOutputLeft = activeEffect->GetAudioLeft();
            effectOutputRight = activeEffect->GetAudioRight();
```
Replace with:
```cpp
            effectOutputLeft = activeEffect->GetAudioLeft();
            effectOutputRight = activeEffect->GetAudioRight();

            // Never let NaN or infinity reach the DAC. Flag it so the main loop can
            // reset the effect, and mute briefly so the recovery does not pop.
            if (audio_guard::SanitizePair(effectOutputLeft, effectOutputRight)) {
                guardTripped = true;
                guardMuteSamplesRemaining = guardMuteTimeInSamples;
            }
```
Then find:
```cpp
        out[0][i] = crossFaderLeft.Process(crossFadeSourceLeft, crossFadeTargetLeft);
        out[1][i] = crossFaderRight.Process(crossFadeSourceRight, crossFadeTargetRight);
```
Replace with:
```cpp
        out[0][i] = crossFaderLeft.Process(crossFadeSourceLeft, crossFadeTargetLeft);
        out[1][i] = crossFaderRight.Process(crossFadeSourceRight, crossFadeTargetRight);

        if (guardMuteSamplesRemaining > 0) {
            guardMuteSamplesRemaining -= 1;
            out[0][i] = 0.0f;
            out[1][i] = 0.0f;
        }
```

- [ ] **Step 4: Recover in the main loop**

In `main()`, after the line that computes `crossFaderTransitionTimeInSamples`, add:
```cpp
    guardMuteTimeInSamples = hardware.GetNumberOfSamplesForTime(guardMuteTimeInSeconds);
```
In the `while (1)` loop, directly after the `// Handle Global Tempo Changes` block, add:
```cpp
        // Recover from non-finite audio: clear the effect's internal state so the bad
        // value cannot keep recirculating. Parameters are untouched.
        if (guardTripped) {
            guardTripped = false;
            guardTripCount += 1;
            activeEffect->Reset();
        }
```

- [ ] **Step 5: Show the trip count on the debug display**

In the `useDebugDisplay` block, find:
```cpp
                sprintf(strbuff, "BPM %ld", globalTempoBPM);
                hardware.display.WriteString(strbuff, Font_7x10, true);
                hardware.display.Update();
```
Replace with:
```cpp
                sprintf(strbuff, "BPM %ld", globalTempoBPM);
                hardware.display.WriteString(strbuff, Font_7x10, true);
                hardware.display.SetCursor(70, 45);
                sprintf(strbuff, "grd %lu", (unsigned long)guardTripCount);
                hardware.display.WriteString(strbuff, Font_7x10, true);
                hardware.display.Update();
```

- [ ] **Step 6: Build and check memory**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/format.sh && make clean >/dev/null && make -j8 2>&1 | grep -E "warning|error|DTCMRAM:|SRAM:"
```
Expected: no warnings or errors. DTCMRAM within 64 bytes of baseline. SRAM within 2 KB of baseline.

- [ ] **Step 7: Hardware check: provoke a runaway**

Flash Ben's pedal (`make program-dfu` with the pedal in bootloader mode). Select Delay, turn D Feedback to maximum, Delay Mix to maximum, strum hard and let it build for 30 seconds, then hold the guitar against the amp for feedback for another 30 seconds. Expected: the pedal keeps responding to footswitches and the encoder throughout. Any dropout is under a tenth of a second. Toggle `useDebugDisplay` is not exposed in the UI, so if you want the count, temporarily set `bool useDebugDisplay = true;` for this test and revert before committing.

- [ ] **Step 8: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/guitar_pedal.cpp
git commit -m "Clamp inputs, sanitize effect output, reset effect on non-finite audio

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Crash record header with host unit tests

Spec section 2.3 diagnostics. This task defines the record layout and its validation as pure code; Task 6 puts it in backup SRAM and writes it from the fault handler.

**Files:**
- Create: `Software/GuitarPedal/Util/crash_record.h`
- Create: `Software/GuitarPedal/tests/test_crash_record.cpp`

**Interfaces:**
- Produces:
  - `struct bkshepherd::CrashRecord { uint32_t magic; uint32_t pc; uint32_t lr; uint32_t cfsr; int32_t effectID; uint32_t uptimeMs; uint32_t checksum; }`, 28 bytes, 4-byte aligned, trivially copyable.
  - `constexpr uint32_t kCrashRecordMagic = 0xC7A5FA17;`
  - `inline uint32_t CrashRecordChecksum(const CrashRecord &r)` XOR of all fields except `checksum`, then XOR with `0xA5A5A5A5` so an all-zero record is invalid.
  - `inline void CrashRecordFill(CrashRecord &r, uint32_t pc, uint32_t lr, uint32_t cfsr, int32_t effectID, uint32_t uptimeMs)` sets magic and fields and computes the checksum.
  - `inline bool CrashRecordIsValid(const CrashRecord &r)` true only if magic matches and checksum matches.
  - `inline void CrashRecordClear(CrashRecord &r)` zeroes the record.
  - Header includes only `<cstdint>` and `<cstring>`.

- [ ] **Step 1: Write the failing tests**

Create `Software/GuitarPedal/tests/test_crash_record.cpp`:
```cpp
// Host tests for Util/crash_record.h.
#include "../Util/crash_record.h"

#include <cstdio>
#include <cstring>

using namespace bkshepherd;

static int failures = 0;
#define CHECK(cond)                                                                                                            \
    do {                                                                                                                       \
        if (!(cond)) {                                                                                                         \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                                                         \
            ++failures;                                                                                                        \
        }                                                                                                                      \
    } while (0)

static void test_zeroed_record_is_invalid() {
    CrashRecord r;
    std::memset(&r, 0, sizeof(r));
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_filled_record_is_valid_and_keeps_fields() {
    CrashRecord r;
    CrashRecordFill(r, 0x24001234u, 0x24000abcu, 0x00008200u, 7, 123456u);
    CHECK(CrashRecordIsValid(r) == true);
    CHECK(r.magic == kCrashRecordMagic);
    CHECK(r.pc == 0x24001234u);
    CHECK(r.lr == 0x24000abcu);
    CHECK(r.cfsr == 0x00008200u);
    CHECK(r.effectID == 7);
    CHECK(r.uptimeMs == 123456u);
}

static void test_corrupted_field_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    r.pc ^= 0x1u;
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_wrong_magic_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    r.magic = 0x12345678u;
    r.checksum = CrashRecordChecksum(r); // even with a consistent checksum, wrong magic fails
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_clear_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    CrashRecordClear(r);
    CHECK(CrashRecordIsValid(r) == false);
    CHECK(r.magic == 0u);
}

static void test_layout_is_28_bytes() { CHECK(sizeof(CrashRecord) == 28); }

int main() {
    test_zeroed_record_is_invalid();
    test_filled_record_is_valid_and_keeps_fields();
    test_corrupted_field_invalidates();
    test_wrong_magic_invalidates();
    test_clear_invalidates();
    test_layout_is_28_bytes();
    if (failures == 0) {
        std::printf("test_crash_record: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make -C tests test_crash_record
```
Expected: compile error, `'../Util/crash_record.h' file not found`.

- [ ] **Step 3: Write the header**

Create `Software/GuitarPedal/Util/crash_record.h`:
```cpp
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
```

- [ ] **Step 4: Run all host tests**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make -C tests
```
Expected: `test_audio_guard: all passed` and `test_crash_record: all passed`, exit code 0.

- [ ] **Step 5: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/Util/crash_record.h Software/GuitarPedal/tests/test_crash_record.cpp
git commit -m "Add CrashRecord layout and validation with host tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Hard fault handler writes the record; startup reports it

Spec section 2.3 diagnostics. libDaisy defines `HardFault_Handler` as a weak alias of `Default_Handler` in `dependencies/libDaisy/core/startup_stm32h750xx.c:1208`, so a strong definition in our sources replaces it. Backup SRAM at `0x38800000` survives resets while power is applied; libDaisy's linker script provides a `.backup_sram` section, and `System::InitBackupSram()` enables the domain. Nothing in libDaisy calls `InitBackupSram()` for us, so `main` must.

**Files:**
- Create: `Software/GuitarPedal/Util/crash_handler.cpp`
- Modify: `Software/GuitarPedal/Makefile` (`CPP_SOURCES`)
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (`main`)

**Interfaces:**
- Consumes: `CrashRecord`, `CrashRecordFill`, `CrashRecordIsValid`, `CrashRecordClear` (Task 5); `activeEffectID` global; `daisy::System::GetNow()`.
- Produces:
  - `extern bkshepherd::CrashRecord g_crashRecord;` placed in `.backup_sram`.
  - `extern "C" void HardFault_Handler(void)` strong definition.
  - Startup behavior: if `g_crashRecord` is valid, print one line over USB serial and blink both LEDs five times, then clear the record.

- [ ] **Step 1: Write the handler**

Create `Software/GuitarPedal/Util/crash_handler.cpp`:
```cpp
// Hard fault handler that leaves a CrashRecord in backup SRAM and then waits for the
// independent watchdog to reboot the pedal. Overrides libDaisy's weak default handler.

#include "crash_record.h"

#include "daisy_seed.h"

extern int activeEffectID; // defined in guitar_pedal.cpp

namespace bkshepherd {
// Backup SRAM is not zeroed at reset, so the record survives a watchdog or fault reboot.
CrashRecord g_crashRecord __attribute__((section(".backup_sram")));
} // namespace bkshepherd

extern "C" {

// Called from the naked handler below with a pointer to the exception stack frame.
// Frame layout: r0, r1, r2, r3, r12, lr, pc, xpsr.
void HardFault_HandlerC(uint32_t *stackFrame) {
    const uint32_t stackedLr = stackFrame[5];
    const uint32_t stackedPc = stackFrame[6];
    bkshepherd::CrashRecordFill(bkshepherd::g_crashRecord, stackedPc, stackedLr, SCB->CFSR, activeEffectID,
                                daisy::System::GetNow());

    // Do not try to recover. The watchdog started in main() will reset the MCU within
    // its timeout, and the next boot reports the record.
    while (true) {
    }
}

// Naked so the stack pointer we inspect is the one the fault pushed onto.
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile("tst lr, #4        \n"
                   "ite eq            \n"
                   "mrseq r0, msp     \n"
                   "mrsne r0, psp     \n"
                   "b HardFault_HandlerC \n");
}
}
```

- [ ] **Step 2: Add it to the build**

In `Software/GuitarPedal/Makefile`, find the line:
```make
CPP_SOURCES += Effect-Modules/tuner_module.cpp
```
Directly after it add:
```make
CPP_SOURCES += Util/crash_handler.cpp
```
(Check the existing `Util/` entries in `CPP_SOURCES` first; if there is a `Util` block, put the line there instead.)

- [ ] **Step 3: Enable backup SRAM and report at startup**

In `guitar_pedal.cpp`, after the includes add:
```cpp
#include "Util/crash_record.h"

namespace bkshepherd {
extern CrashRecord g_crashRecord;
}
```
In `main()`, directly after `hardware.Init(blockSize, boost);` add:
```cpp
    // Backup SRAM holds the last crash record across reboots. Enable it before reading.
    System::InitBackupSram();

    // Report a crash from the previous run: one line over USB serial and five fast
    // blinks of both LEDs, then clear the record so it is reported only once.
    if (CrashRecordIsValid(g_crashRecord)) {
        hardware.seed.StartLog(false);
        hardware.seed.PrintLine("CRASH pc=0x%08lx lr=0x%08lx cfsr=0x%08lx effect=%ld uptime=%lums",
                                (unsigned long)g_crashRecord.pc, (unsigned long)g_crashRecord.lr,
                                (unsigned long)g_crashRecord.cfsr, (long)g_crashRecord.effectID,
                                (unsigned long)g_crashRecord.uptimeMs);
        for (int i = 0; i < 5; i++) {
            hardware.SetLed(0, 1.0f);
            hardware.SetLed(1, 1.0f);
            hardware.UpdateLeds();
            System::Delay(80);
            hardware.SetLed(0, 0.0f);
            hardware.SetLed(1, 0.0f);
            hardware.UpdateLeds();
            System::Delay(80);
        }
        CrashRecordClear(g_crashRecord);
    }
```

- [ ] **Step 4: Build all variants**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/format.sh && for v in 125B 1590B 1590B_SMD TERRARIUM FUNBOX; do make clean >/dev/null && make -j8 VARIANT=$v 2>&1 | grep -E "warning|error|SRAM:"; done
```
Expected: five `SRAM:` lines, no warnings or errors. If the linker complains about `.backup_sram`, confirm the app is built with `APP_TYPE = BOOT_SRAM` (the sram linker script has the section; the internal-flash script may not).

- [ ] **Step 5: Hardware check with a forced fault**

Temporarily add to the top of the `while (1)` loop in `main()`:
```cpp
        if (hardware.switches[hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate)].TimeHeldMs() > 5000) {
            volatile uint32_t *bad = reinterpret_cast<volatile uint32_t *>(0xFFFFFFF0u);
            *bad = 1; // deliberate bus fault -> hard fault
        }
```
Build, flash, hold Alt for 5 seconds. The pedal freezes (the watchdog is not in yet; that is Task 7). Power cycle. Expected: both LEDs blink five times at startup, then normal operation. Connect the pedal's USB to the Mac and open a serial terminal within a second of power-up if you want the text line:
```bash
ls /dev/tty.usbmodem*
screen /dev/tty.usbmodem* 115200
```
Remove the temporary block before committing and confirm with `git diff` that only the intended changes remain.

- [ ] **Step 6: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/Util/crash_handler.cpp Software/GuitarPedal/Makefile Software/GuitarPedal/guitar_pedal.cpp
git commit -m "Record hard faults in backup SRAM and report them at next boot

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Independent watchdog

Spec section 2.3, mitigation 3. The IWDG runs from the 32 kHz LSI clock. Prescaler 256 gives a 125 Hz tick, so a reload of 250 is a 2-second timeout. libDaisy's HAL configuration already enables the IWDG module (`HAL_IWDG_MODULE_ENABLED` in `stm32h7xx_hal_conf.h:67`).

**Files:**
- Create: `Software/GuitarPedal/Util/watchdog.h`
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (`main`)

**Interfaces:**
- Produces:
  - `void bkshepherd::WatchdogStart(float timeoutSeconds)` starts the IWDG. Once started it cannot be stopped until reset.
  - `void bkshepherd::WatchdogKick()` refreshes it. Must be called at least every `timeoutSeconds`.
  - `constexpr bool kEnableWatchdog = true;` in `guitar_pedal.cpp`. Set to `false` when running under a debugger, because a halted core is not kicking the dog.

- [ ] **Step 1: Write the wrapper**

Create `Software/GuitarPedal/Util/watchdog.h`:
```cpp
#pragma once
#ifndef WATCHDOG_H
#define WATCHDOG_H

// Thin wrapper over the STM32H7 independent watchdog (IWDG1).
// Once started the watchdog cannot be disabled; only a reset clears it.

#include "stm32h7xx_hal.h"

namespace bkshepherd {

inline IWDG_HandleTypeDef &WatchdogHandle() {
    static IWDG_HandleTypeDef handle;
    return handle;
}

/** Start the watchdog with the given timeout. LSI is 32 kHz; with prescaler 256 one tick
 *  is 8 ms, and the reload register is 12 bits, so the maximum timeout is about 32 s. */
inline void WatchdogStart(float timeoutSeconds) {
    constexpr float kTickSeconds = 256.0f / 32000.0f;
    uint32_t reload = static_cast<uint32_t>(timeoutSeconds / kTickSeconds);
    if (reload < 1) {
        reload = 1;
    }
    if (reload > 0x0FFF) {
        reload = 0x0FFF;
    }

    IWDG_HandleTypeDef &h = WatchdogHandle();
    h.Instance = IWDG1;
    h.Init.Prescaler = IWDG_PRESCALER_256;
    h.Init.Reload = reload;
    h.Init.Window = IWDG_WINDOW_DISABLE;
    HAL_IWDG_Init(&h);
}

inline void WatchdogKick() { HAL_IWDG_Refresh(&WatchdogHandle()); }

} // namespace bkshepherd

#endif
```

- [ ] **Step 2: Start it and kick it**

In `guitar_pedal.cpp`, after the other `Util/` includes add:
```cpp
#include "Util/watchdog.h"
```
Near the top of the file, after `bool useDebugDisplay = false;` add:
```cpp
// Set to false when debugging with a halted core, otherwise the watchdog resets the pedal.
constexpr bool kEnableWatchdog = true;
constexpr float kWatchdogTimeoutSeconds = 2.0f;
```
In `main()`, directly before `// start callback`, add:
```cpp
    // A hang or hard fault now becomes a 2-second reboot instead of a frozen pedal.
    if (kEnableWatchdog) {
        WatchdogStart(kWatchdogTimeoutSeconds);
    }
```
As the first statement inside `while (1) {` add:
```cpp
        if (kEnableWatchdog) {
            WatchdogKick();
        }
```

- [ ] **Step 3: Build all variants**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && ./ci/format.sh && for v in 125B 1590B 1590B_SMD TERRARIUM FUNBOX; do make clean >/dev/null && make -j8 VARIANT=$v 2>&1 | grep -E "warning|error|SRAM:"; done
```
Expected: five `SRAM:` lines, no warnings or errors. If `IWDG1` or `IWDG_PRESCALER_256` is undeclared, the HAL header path is missing from includes; `daisy_seed.h` pulls in `stm32h7xx_hal.h`, so include `"daisy_seed.h"` at the top of `watchdog.h` instead of the HAL header directly.

- [ ] **Step 4: Hardware check: fault now self-recovers**

Re-add the temporary forced-fault block from Task 6 Step 5, build, flash, hold Alt for 5 seconds. Expected: the pedal goes silent, and within about 2 seconds reboots on its own, both LEDs blink five times, and normal operation resumes without a power cycle. Remove the temporary block and confirm with `git diff`.

Also confirm the watchdog does not fire in normal use: leave the pedal running for 10 minutes while playing, switching effects, and opening every menu including Preset > Erase All. It must never reboot (no five-blink pattern, no gap in audio).

- [ ] **Step 5: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add Software/GuitarPedal/Util/watchdog.h Software/GuitarPedal/guitar_pedal.cpp
git commit -m "Add independent watchdog so hangs and faults reboot instead of freezing

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Flashing guides for macOS and Windows

Spec section 7, item 2. Both guides lead with the browser-based path, then give the command-line path, then first-time bootloader installation, then troubleshooting based on what happened on 2026-09-23.

**Files:**
- Create: `docs/FLASHING-MAC.md`
- Create: `docs/FLASHING-WINDOWS.md`
- Modify: `README.md` (add a "Flashing" section near the top that links both)

**Interfaces:**
- None. Documentation only.

- [ ] **Step 1: Write the macOS guide**

Create `docs/FLASHING-MAC.md`:
````markdown
# Flashing the pedal from a Mac

This installs a firmware `.bin` onto the Daisy Seed inside the pedal. It takes about five minutes the first time and under a minute after that.

You need: the pedal, a USB micro-B cable that carries data (some charging cables do not), and a Mac with Chrome or Edge for the browser method, or Homebrew for the command-line method.

## Which file to flash

Every push to `main` builds firmware for all pedal variants on GitHub.

1. Open the repository's **Actions** tab and click the newest **Build All** run with a green check.
2. Scroll to **Artifacts** and download the one for your pedal. For the 125B build (the one Ben and Steve have) that is **125B-Firmware**.
3. Unzip it. You get `125B.bin`.

If you built the firmware yourself, the file is `Software/GuitarPedal/build/guitarpedal.bin`.

## Method 1: browser (recommended)

1. Plug the pedal into the Mac by USB. Power the pedal from its 9 V supply as usual.
2. Open https://flash.daisy.audio in Chrome or Edge. Safari and Firefox do not support WebUSB.
3. Put the pedal in bootloader mode. Press **RESET** on the Daisy Seed, then within 5 seconds single-press **BOOT**. The Seed's onboard LED keeps blinking; that means the bootloader is locked open and will wait for you.
4. In the web flasher, choose **Connect**, pick the device named **DFU in FS Mode** or **Daisy Bootloader**, and allow it.
5. Under the firmware section choose **File Upload**, pick your `.bin`, and press **Flash**. The progress bar runs for a few seconds.
6. When it reports done, press **RESET** on the Seed. The pedal starts on the new firmware.

## Method 2: command line with dfu-util

1. Install dfu-util once:

   ```bash
   brew install dfu-util
   ```

2. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**. The onboard LED keeps blinking.
3. Confirm the Mac sees it:

   ```bash
   dfu-util -l
   ```

   You should see one line containing `0483:df11` whose `name=` starts with `@Flash /0x90000000`. That is the Daisy bootloader. If the name starts with `@Internal Flash /0x08000000` instead, see troubleshooting below.

4. Flash, replacing the path with your file:

   ```bash
   dfu-util -a 0 -s 0x90040000:leave -D ~/Downloads/125B.bin -d ,0483:df11
   ```

   Expected output ends with `File downloaded successfully` and `Transitioning to dfuMANIFEST state`. The pedal reboots by itself.

   The warning `Invalid DFU suffix signature` is harmless.

## First-time only: installing the Daisy bootloader

Skip this if the pedal has ever been flashed before; the bootloader is already there. A brand-new Daisy Seed needs it once.

1. Hold **BOOT**, press and release **RESET**, then release **BOOT**. This enters the factory bootloader.
2. In the web flasher, open the **Bootloader** tab and press **Flash Bootloader**. Or from the command line, from `Software/GuitarPedal` after building:

   ```bash
   make program-boot
   ```

3. Press **RESET**. The onboard LED blinks for about 3 seconds. Now follow Method 1 or 2 above.

## Troubleshooting

**`dfu-util -l` shows `@Internal Flash /0x08000000` and flashing fails with `Last page at ... is not writeable`.**
You entered the factory bootloader by holding BOOT while pressing RESET. Press RESET on its own, then single-press BOOT within 5 seconds. Run `dfu-util -l` again; the name should now start with `@Flash /0x90000000`.

**`No DFU capable USB device available`, or the device disappears after a couple of seconds.**
RESET was pressed but BOOT was not, so the bootloader timed out and started the old firmware. Press RESET, then BOOT within 5 seconds, and retry.

**Nothing shows up at all.**
Try another USB cable; many are charge-only. Try a USB-A port or a different hub. Make sure the pedal is powered.

**The pedal starts but the LEDs are off.**
Firmware older than September 2026 booted bypassed. Tap the right footswitch. If the LED still does not light, flash the newest build from the Actions tab.
````

- [ ] **Step 2: Write the Windows guide**

Create `docs/FLASHING-WINDOWS.md`:
````markdown
# Flashing the pedal from Windows

This installs a firmware `.bin` onto the Daisy Seed inside the pedal. Plan on ten minutes the first time, mostly for the USB driver, and under a minute after that.

You need: the pedal, a USB micro-B cable that carries data (some charging cables do not), and a PC with Chrome or Edge.

## Which file to flash

Every push to `main` builds firmware for all pedal variants on GitHub.

1. Open the repository's **Actions** tab and click the newest **Build All** run with a green check.
2. Scroll to **Artifacts** and download the one for your pedal. For the 125B build (the one Ben and Steve have) that is **125B-Firmware**.
3. Unzip it. You get `125B.bin`.

## One-time driver setup with Zadig

Windows does not ship a driver that lets the browser or dfu-util talk to the Daisy. Zadig installs one. This is the step that trips most people, so do it first.

1. Download Zadig from https://zadig.akeo.ie and run it. No install needed.
2. Plug the pedal in and put it in bootloader mode: press **RESET** on the Daisy Seed, then within 5 seconds single-press **BOOT**. The Seed's onboard LED keeps blinking.
3. In Zadig, choose **Options > List All Devices**.
4. In the dropdown pick the entry named **DFU in FS Mode** or **Daisy Bootloader**. Check that the USB ID shows `0483 DF11`.
5. In the driver box to the right of the arrow, choose **WinUSB**. Press **Replace Driver** (or **Install Driver**). Wait for it to finish.
6. Unplug and replug the pedal. Repeat step 2 to get back into bootloader mode.

If you also want the factory bootloader to work (needed only for a brand-new Seed), repeat steps 3 to 5 once more with the pedal in factory bootloader mode: hold **BOOT**, press and release **RESET**, release **BOOT**. It appears as **STM32 BOOTLOADER**.

## Method 1: browser (recommended)

1. With the driver installed, plug the pedal in and power it from its 9 V supply.
2. Open https://flash.daisy.audio in Chrome or Edge. Firefox does not support WebUSB.
3. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**. The onboard LED keeps blinking.
4. Choose **Connect**, pick **DFU in FS Mode** or **Daisy Bootloader**, and allow it.
5. Under the firmware section choose **File Upload**, pick your `.bin`, and press **Flash**.
6. When it reports done, press **RESET**. The pedal starts on the new firmware.

## Method 2: command line with dfu-util

1. Download the Windows binaries from http://dfu-util.sourceforge.net/releases/ (the newest `dfu-util-x.xx-binaries.tar.xz`). Extract `win64\dfu-util.exe` somewhere convenient, such as `C:\dfu\`.
2. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**.
3. Open PowerShell and check the device is visible:

   ```powershell
   C:\dfu\dfu-util.exe -l
   ```

   Expect one line containing `0483:df11` whose `name=` starts with `@Flash /0x90000000`. If it starts with `@Internal Flash /0x08000000`, see troubleshooting.

4. Flash, replacing the path with your file:

   ```powershell
   C:\dfu\dfu-util.exe -a 0 -s 0x90040000:leave -D "$env:USERPROFILE\Downloads\125B.bin" -d ,0483:df11
   ```

   Expected output ends with `File downloaded successfully`. The pedal reboots by itself. The warning `Invalid DFU suffix signature` is harmless.

## First-time only: installing the Daisy bootloader

Skip this if the pedal has ever been flashed before. A brand-new Daisy Seed needs it once.

1. Hold **BOOT**, press and release **RESET**, then release **BOOT**. This enters the factory bootloader (make sure Zadig has installed WinUSB for **STM32 BOOTLOADER** as described above).
2. In the web flasher open the **Bootloader** tab and press **Flash Bootloader**.
3. Press **RESET**. The onboard LED blinks for about 3 seconds. Now follow Method 1 or 2.

## Troubleshooting

**Zadig does not list the device.**
Choose **Options > List All Devices** and make sure the pedal is in bootloader mode at that moment (RESET, then BOOT within 5 seconds). Try another cable and a USB-A port directly on the PC.

**The browser's Connect dialog is empty.**
The WinUSB driver is not installed for this mode of the device. Run Zadig again with the pedal in bootloader mode.

**`dfu-util -l` shows `@Internal Flash /0x08000000` and flashing fails with `Last page at ... is not writeable`.**
You entered the factory bootloader by holding BOOT while pressing RESET. Press RESET on its own, then single-press BOOT within 5 seconds, and run the flash again.

**`No DFU capable USB device available`, or the device disappears after a couple of seconds.**
RESET was pressed but BOOT was not, so the bootloader timed out and started the old firmware. Press RESET, then BOOT within 5 seconds, and retry.

**The pedal starts but the LEDs are off.**
Firmware older than September 2026 booted bypassed. Tap the right footswitch. If the LED still does not light, flash the newest build from the Actions tab.
````

- [ ] **Step 3: Link from the README**

In the top-level `README.md`, after the first paragraph under the main title, add:
```markdown
## Flashing a pedal

Step-by-step guides, including where to download prebuilt firmware and how to fix the usual USB problems:

- [Flashing from a Mac](docs/FLASHING-MAC.md)
- [Flashing from Windows](docs/FLASHING-WINDOWS.md)
```

- [ ] **Step 4: Verify the Mac guide against a real flash**

Follow `docs/FLASHING-MAC.md` Method 2 literally on Ben's pedal using the `build/guitarpedal.bin` from Task 7, reading each step aloud as written. Anything that does not match what the terminal shows gets corrected in the guide before committing. Also check every link opens.

- [ ] **Step 5: Commit**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git add docs/FLASHING-MAC.md docs/FLASHING-WINDOWS.md README.md
git commit -m "Add step-by-step flashing guides for macOS and Windows

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: CI, memory check, and Steve's pedal

**Files:**
- None new. This task pushes the branch and verifies the CI build.

**Interfaces:**
- Produces: a green **Build All** run on `feature/next` and its `125B-Firmware` artifact, which is what goes on Steve's pedal.

- [ ] **Step 1: Run the host tests and a clean local build one last time**

Run:
```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make -C tests && make clean >/dev/null && make -j8 2>&1 | grep -E "warning|error|DTCMRAM:|SRAM:"
```
Expected: both test binaries pass. DTCMRAM under 95 percent, SRAM under 90 percent. Compare to the Task 0 numbers; the delta should be under 200 bytes DTCM and under 6 KB SRAM. If larger, find what grew with `arm-none-eabi-nm --size-sort build/guitarpedal.elf | tail -20` before continuing.

- [ ] **Step 2: Push and watch CI**

```bash
cd ~/DaisySeedProjects/.worktrees/next && git push -u origin feature/next
gh run list -R benkhsieh/DaisySeedProjects --branch feature/next --limit 1
```
Wait for the run to finish (about 5 minutes):
```bash
gh run watch -R benkhsieh/DaisySeedProjects $(gh run list -R benkhsieh/DaisySeedProjects --branch feature/next --limit 1 --json databaseId -q '.[0].databaseId')
```
Expected: `Build All` succeeds with five artifacts.

- [ ] **Step 3: Flash Ben's pedal from the CI artifact and run the full checklist**

```bash
mkdir -p ~/Downloads/pedal-fw && cd ~/Downloads/pedal-fw && gh run download -R benkhsieh/DaisySeedProjects $(gh run list -R benkhsieh/DaisySeedProjects --branch feature/next --limit 1 --json databaseId -q '.[0].databaseId') -n 125B-Firmware && ls -la
```
Put the pedal in bootloader mode (RESET, then BOOT within 5 seconds) and:
```bash
dfu-util -a 0 -s 0x90040000:leave -D ~/Downloads/pedal-fw/125B.bin -d ,0483:df11
```
Run every hardware check from Tasks 2, 4, 6, and 7 on this exact binary. All must pass.

- [ ] **Step 4: Flash Steve's pedal**

Same command with Steve's pedal. Then on Steve's pedal:
1. Hold Bypass 2 seconds, play a note, confirm the tuner shows it.
2. Switch effects with the encoder, confirm LED 0 stays lit.
3. Delay at full feedback for a minute, confirm no freeze.

Record the outcome of each in the PR description.

- [ ] **Step 5: Open the pull request**

```bash
cd ~/DaisySeedProjects/.worktrees/next && gh pr create -R benkhsieh/DaisySeedProjects --base main --head feature/next --title "Phase 1: tuner and LED fixes, crash guard, watchdog, flashing guides" --body "$(cat <<'EOF'
Implements sections 2 and 7 (flashing guides) of docs/superpowers/specs/2026-09-26-firmware-next-design.md.

- SetActiveEffect now owns enable state and tuner on/off state, fixing the tuner receiving no audio when reached via menu/encoder/MIDI, and LEDs going dark after switching effects.
- Audio guard: input clamp, non-finite output sanitizer, BaseEffectModule::Reset() hook (Delay clears its lines).
- Hard fault handler records pc/lr/cfsr/effect in backup SRAM; next boot blinks and prints it.
- Independent watchdog, 2 s.
- docs/FLASHING-MAC.md and docs/FLASHING-WINDOWS.md.

Hardware checks on Ben's and Steve's 125B pedals: (fill in from Task 9 steps 3 and 4)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-review against the spec

- Section 2.1 tuner: Task 2. Includes the "left input only, signal level" follow-up as a recorded observation in Step 6.
- Section 2.2 LEDs: Task 2.
- Section 2.3 mitigations 1, 2, 3: Tasks 4 (sanitizer, clamp, reset via Task 3) and 7 (watchdog). Diagnostics: Tasks 5 and 6. The spec's "mute for 20 ms" is `guardMuteTimeInSeconds = 0.02f`.
- Section 7 item 2 flashing guides: Task 8, including both failure modes from 2026-09-23 in troubleshooting.
- Section 8 testing for these items: Tasks 2, 4, 6, 7, 9 hardware checks; CI and memory limits in Task 9.
- Section 9 sequence item 1: this plan. Items 2 to 6 are separate plans.
- Names used consistently: `SetActiveEffect`, `effectOnBeforeTuner`, `BaseEffectModule::Reset`, `audio_guard::ClampInput`, `audio_guard::SanitizePair`, `CrashRecord`, `g_crashRecord`, `WatchdogStart`, `WatchdogKick`, `kEnableWatchdog`, `guardTripped`, `guardTripCount`, `guardMuteSamplesRemaining`.
