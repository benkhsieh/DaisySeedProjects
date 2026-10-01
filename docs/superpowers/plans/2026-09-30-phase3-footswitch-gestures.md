# Phase 3: Footswitch Gesture State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the scattered footswitch timers in the audio callback with one small, host-tested state machine that recognizes tap, hold, double-tap, both-tap, and both-hold, and use both-tap to jump to the next effect (spec section 3).

**Architecture:** `Util/footswitch_gestures.h` is a pure header (no Daisy includes) fed once per audio block with the two debounced switch states and the block length; it returns a bitmask of events. `guitar_pedal.cpp` maps events to the existing actions (toggle, tuner quick switch, module Alt callbacks, tap tempo, save, pending effect switch) and nothing else changes in how those actions work. Every effect switch, from any route, now mutes for 20 ms via the guard mute that already exists.

**Tech Stack:** C++20, STM32H750, libDaisy v8 (`Switch::Pressed()`, debounced per block), host tests via `Software/GuitarPedal/tests/Makefile` (`-O3 -ffast-math`).

Spec: `docs/superpowers/specs/2026-09-26-firmware-next-design.md` section 3 (gesture table) and section 5.1's "Both tap advances to the next effect in the active group"; groups arrive in Phase 4, so this phase steps through the full list.

Baseline: commit 4bb7c0f (main 18c5829), 125B: DTCMRAM 103968 B, SRAM 402428 B.

## Global Constraints

- The gesture header includes only `<cstdint>` and `<algorithm>`; it is a class with value members only (no allocation, no statics), instantiated once as a file-scope object in `guitar_pedal.cpp` (a few dozen bytes of DTCM; DTCMRAM must stay at or below 104100 B).
- Gesture timings are named constants in the header's `Config` with the spec's values: both-press window 80 ms, bypass hold 2000 ms, Alt hold 1000 ms, double-tap window 2000 ms, both-hold 2000 ms.
- Behavior that must not change: a normal Bypass tap toggles within one block after the 80 ms window; the tuner quick switch (hold Bypass 2 s) still undoes the tap's toggle and defers the switch to the main loop via `pendingEffectID`; Alt events are delivered to the module only while the effect is on; tap tempo uses `s_to_tempo(seconds)`; saving requires both footswitches held 2 s; the single-footswitch variants (`has_alternate_footswitch == false`) save on a 2 s Bypass hold.
- No `git stash`; `clang-format -i` only on changed files; commit trailer `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`; branch `feature/next`, worktree `~/DaisySeedProjects/.worktrees/next`. Five variants build. Known noise: `"PI" redefined` and `dependencies/` warnings.
- Right footswitch is Bypass, left is Alt.

---

## File Structure

| Path | Responsibility |
|---|---|
| `Software/GuitarPedal/Util/footswitch_gestures.h` | New. `FootswitchGestures` class: per-block update, event bitmask, double-tap interval. |
| `Software/GuitarPedal/tests/test_footswitch_gestures.cpp`, `tests/Makefile` | New scripted tests of the gesture table; Makefile target. |
| `Software/GuitarPedal/guitar_pedal.cpp` | Modify. Replace the footswitch block in `AudioCallback` and the related globals/init with the state machine and an event-to-action map; mute on effect switch; debug screen shows the last gesture. |

---

### Task 1: Gesture state machine with host tests

**Files:**
- Create: `Software/GuitarPedal/Util/footswitch_gestures.h`, `Software/GuitarPedal/tests/test_footswitch_gestures.cpp`
- Modify: `Software/GuitarPedal/tests/Makefile` (add `test_footswitch_gestures` to `TESTS` and a rule mapping it to `../Util/footswitch_gestures.h`)

**Interfaces (produced):**
```cpp
namespace bkshepherd {
class FootswitchGestures {
  public:
    enum Event : uint32_t {
        kNone = 0,
        kBypassTap = 1u << 0,    // toggle effect
        kBypassHold = 1u << 1,   // tuner quick switch (once per press)
        kAltPress = 1u << 2,     // module AlternateFootswitchPressed
        kAltRelease = 1u << 3,   // module AlternateFootswitchReleased
        kAltDoubleTap = 1u << 4, // tap tempo; see LastDoubleTapIntervalMs()
        kAltHold = 1u << 5,      // module AlternateFootswitchHeldFor1Second (every block while held)
        kBothTap = 1u << 6,      // next effect (emitted on the first release)
        kBothHold = 1u << 7,     // save preset (once)
    };
    struct Config {
        float bothWindowMs = 80.0f;
        float bypassHoldMs = 2000.0f;
        float altHoldMs = 1000.0f;
        float doubleTapWindowMs = 2000.0f;
        float bothHoldMs = 2000.0f;
    };
    void Init(const Config &config);
    uint32_t Update(bool bypassDown, bool altDown, float dtMs); // call once per block, after the switches are debounced
    float LastDoubleTapIntervalMs() const;
};
}
```
Semantics (the spec's table): a switch press is "committed" as an individual event when it has been down for `bothWindowMs`, or on release if sooner. If the other switch goes down while the first is still inside its window, both presses are consumed by a both-gesture and no individual events fire. Both-hold fires once at `bothHoldMs`; both-tap fires on the first release if both-hold has not fired. Bypass hold fires once at `bypassHoldMs` only when Alt is up. Alt hold fires every block from `altHoldMs` on. Alt double-tap fires on the second committed Alt press whose rise came within `doubleTapWindowMs` of the previous Alt rise; the interval is between rises.

- [ ] **Step 1: Failing tests**

```cpp
// Scripted tests for Util/footswitch_gestures.h. Time advances in 1 ms blocks.
#include "../Util/footswitch_gestures.h"
#include <cstdio>
#include <vector>
using namespace bkshepherd;
using G = FootswitchGestures;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Rec { float t; uint32_t ev; };
struct Script {
    G g; float t = 0; std::vector<Rec> log;
    Script() { g.Init(G::Config{}); }
    // Hold the given states for ms, recording every non-empty event mask with its time.
    void run(bool byp, bool alt, float ms) {
        for (float e = 0; e < ms; e += 1.0f) { t += 1.0f; uint32_t ev = g.Update(byp, alt, 1.0f); if (ev) log.push_back({t, ev}); }
    }
    int count(uint32_t mask) const { int n = 0; for (auto &r : log) if (r.ev & mask) ++n; return n; }
    float first(uint32_t mask) const { for (auto &r : log) if (r.ev & mask) return r.t; return -1; }
};

static void test_quick_bypass_tap_fires_on_release() {
    Script s; s.run(true, false, 30); s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1); CHECK(s.first(G::kBypassTap) == 31);
    CHECK(s.count(G::kBypassHold) == 0); CHECK(s.count(G::kBothTap) == 0);
}
static void test_bypass_tap_fires_at_window_when_held_longer() {
    Script s; s.run(true, false, 200); s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1); CHECK(s.first(G::kBypassTap) == 80);
}
static void test_bypass_hold_fires_once_at_2s() {
    Script s; s.run(true, false, 2500); s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1); CHECK(s.count(G::kBypassHold) == 1); CHECK(s.first(G::kBypassHold) == 2000);
}
static void test_alt_tap_press_and_release() {
    Script s; s.run(false, true, 40); s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 1); CHECK(s.count(G::kAltRelease) == 1); CHECK(s.count(G::kAltDoubleTap) == 0);
    CHECK(s.first(G::kAltRelease) >= s.first(G::kAltPress));
}
static void test_alt_double_tap_reports_interval_between_rises() {
    Script s; s.run(false, true, 40); s.run(false, false, 460); s.run(false, true, 40); s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 2); CHECK(s.count(G::kAltDoubleTap) == 1);
    CHECK(s.g.LastDoubleTapIntervalMs() == 500.0f);
}
static void test_alt_taps_far_apart_are_not_a_double_tap() {
    Script s; s.run(false, true, 40); s.run(false, false, 2500); s.run(false, true, 40); s.run(false, false, 50);
    CHECK(s.count(G::kAltDoubleTap) == 0);
}
static void test_alt_hold_fires_every_block_after_1s() {
    Script s; s.run(false, true, 1500); s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 1); CHECK(s.first(G::kAltHold) == 1000);
    CHECK(s.count(G::kAltHold) == 501); CHECK(s.count(G::kAltRelease) == 1);
}
static void test_both_tap_suppresses_individual_events_and_fires_on_release() {
    Script s; s.run(true, false, 30); s.run(true, true, 270); s.run(false, true, 20); s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 0); CHECK(s.count(G::kAltPress) == 0); CHECK(s.count(G::kAltRelease) == 0);
    CHECK(s.count(G::kBothTap) == 1); CHECK(s.first(G::kBothTap) == 301); CHECK(s.count(G::kBothHold) == 0);
}
static void test_both_hold_fires_once_and_no_both_tap_after() {
    Script s; s.run(false, true, 20); s.run(true, true, 2480); s.run(true, false, 100); s.run(false, false, 50);
    CHECK(s.count(G::kBothHold) == 1); CHECK(s.first(G::kBothHold) == 2020); CHECK(s.count(G::kBothTap) == 0);
    CHECK(s.count(G::kBypassTap) == 0); CHECK(s.count(G::kAltPress) == 0);
}
static void test_second_switch_outside_window_is_two_separate_presses() {
    Script s; s.run(true, false, 200); s.run(true, true, 200); s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1); CHECK(s.count(G::kAltPress) == 1); CHECK(s.count(G::kBothTap) == 0);
    CHECK(s.count(G::kBypassHold) == 0);
}
static void test_bypass_hold_blocked_while_alt_down() {
    Script s; s.run(true, false, 200); s.run(true, true, 2500); s.run(false, false, 50);
    CHECK(s.count(G::kBypassHold) == 0);
}
static void test_new_press_after_both_gesture_works_normally() {
    Script s; s.run(true, true, 100); s.run(false, false, 100); s.run(true, false, 30); s.run(false, false, 50);
    CHECK(s.count(G::kBothTap) == 1); CHECK(s.count(G::kBypassTap) == 1);
}
int main() {
    test_quick_bypass_tap_fires_on_release(); test_bypass_tap_fires_at_window_when_held_longer(); test_bypass_hold_fires_once_at_2s();
    test_alt_tap_press_and_release(); test_alt_double_tap_reports_interval_between_rises(); test_alt_taps_far_apart_are_not_a_double_tap();
    test_alt_hold_fires_every_block_after_1s(); test_both_tap_suppresses_individual_events_and_fires_on_release();
    test_both_hold_fires_once_and_no_both_tap_after(); test_second_switch_outside_window_is_two_separate_presses();
    test_bypass_hold_blocked_while_alt_down(); test_new_press_after_both_gesture_works_normally();
    if (!failures) std::printf("test_footswitch_gestures: all passed\n");
    return failures ? 1 : 0;
}
```
Timing convention for the expected values: `Update` advances time by `dtMs` first, then evaluates. A press that starts at t=1 (first block with the switch down) has `heldMs = 80` at t=80, so a window-committed tap fires at t=80. In the first test, the release block is t=31 (press blocks 1..30, first up block 31) and the tap fires there. In the both-hold test, Alt rises at t=1 and Bypass at t=21; both-hold fires when the both-gesture has lasted 2000 ms, measured from the second rise, so at t=2020. Alt hold fires at every block from t=1000 through t=1500 inclusive: 501 blocks.

- [ ] **Step 2: Header**

```cpp
#pragma once
#ifndef FOOTSWITCH_GESTURES_H
#define FOOTSWITCH_GESTURES_H

#include <algorithm>
#include <cstdint>

// Footswitch gesture recognizer. Header-only, no Daisy includes, so tests/ can build it on
// a host machine. Fed once per audio block with the debounced switch states; returns a
// bitmask of the gestures that completed in that block.
//
// Individual presses are "committed" after bothWindowMs (or on release if sooner). If the
// other switch goes down while the first is still inside its window, both presses are
// consumed by a both-gesture and no individual events fire for them.

namespace bkshepherd {

class FootswitchGestures {
  public:
    enum Event : uint32_t {
        kNone = 0,
        kBypassTap = 1u << 0,
        kBypassHold = 1u << 1,
        kAltPress = 1u << 2,
        kAltRelease = 1u << 3,
        kAltDoubleTap = 1u << 4,
        kAltHold = 1u << 5,
        kBothTap = 1u << 6,
        kBothHold = 1u << 7,
    };

    struct Config {
        float bothWindowMs = 80.0f;
        float bypassHoldMs = 2000.0f;
        float altHoldMs = 1000.0f;
        float doubleTapWindowMs = 2000.0f;
        float bothHoldMs = 2000.0f;
    };

    void Init(const Config &config) {
        m_cfg = config;
        m_bypass = SwitchState{};
        m_alt = SwitchState{};
        m_both = false;
        m_bothHeldMs = 0.0f;
        m_bothHoldEmitted = false;
        m_bothTapArmed = false;
        m_sinceLastAltRiseMs = kFarPast;
        m_altRiseInterval = kFarPast;
        m_lastDoubleTapIntervalMs = 0.0f;
    }

    float LastDoubleTapIntervalMs() const { return m_lastDoubleTapIntervalMs; }

    uint32_t Update(bool bypassDown, bool altDown, float dtMs) {
        uint32_t ev = kNone;

        // Advance time.
        if (m_bypass.down) m_bypass.heldMs += dtMs;
        if (m_alt.down) m_alt.heldMs += dtMs;
        if (m_both) m_bothHeldMs += dtMs;
        m_sinceLastAltRiseMs = std::min(m_sinceLastAltRiseMs + dtMs, kFarPast);

        // Edges.
        const bool bypassRise = bypassDown && !m_bypass.down;
        const bool bypassFall = !bypassDown && m_bypass.down;
        const bool altRise = altDown && !m_alt.down;
        const bool altFall = !altDown && m_alt.down;

        if (bypassRise) m_bypass.Press();
        if (altRise) {
            m_alt.Press();
            m_altRiseInterval = m_sinceLastAltRiseMs; // time since the previous Alt rise
            m_sinceLastAltRiseMs = 0.0f;
        }

        // Both-gesture: the second switch rises while the first is down and still inside
        // its window and not yet committed as an individual press.
        if (!m_both && m_bypass.down && m_alt.down && !m_bypass.emitted && !m_alt.emitted && !m_bypass.consumed &&
            !m_alt.consumed && m_bypass.heldMs <= m_cfg.bothWindowMs && m_alt.heldMs <= m_cfg.bothWindowMs) {
            m_both = true;
            m_bothHeldMs = 0.0f;
            m_bothHoldEmitted = false;
            m_bothTapArmed = true;
            m_bypass.consumed = true;
            m_alt.consumed = true;
        }

        // Commit individual presses.
        if (m_bypass.down && !m_bypass.emitted && !m_bypass.consumed && (m_bypass.heldMs >= m_cfg.bothWindowMs || bypassFall)) {
            m_bypass.emitted = true;
            ev |= kBypassTap;
        }
        if (m_alt.down && !m_alt.emitted && !m_alt.consumed && (m_alt.heldMs >= m_cfg.bothWindowMs || altFall)) {
            m_alt.emitted = true;
            ev |= kAltPress;
            if (m_altRiseInterval <= m_cfg.doubleTapWindowMs) {
                ev |= kAltDoubleTap;
                m_lastDoubleTapIntervalMs = m_altRiseInterval;
            }
        }

        // Holds.
        if (m_bypass.down && m_bypass.emitted && !m_bypass.consumed && !m_bypass.holdEmitted && !m_alt.down &&
            m_bypass.heldMs >= m_cfg.bypassHoldMs) {
            m_bypass.holdEmitted = true;
            ev |= kBypassHold;
        }
        if (m_alt.down && m_alt.emitted && !m_alt.consumed && m_alt.heldMs >= m_cfg.altHoldMs) {
            ev |= kAltHold;
        }

        // Release of a committed Alt press.
        if (altFall && m_alt.emitted && !m_alt.consumed) {
            ev |= kAltRelease;
        }

        // Both-gesture completion.
        if (m_both) {
            if (!m_bothHoldEmitted && m_bothHeldMs >= m_cfg.bothHoldMs) {
                m_bothHoldEmitted = true;
                m_bothTapArmed = false;
                ev |= kBothHold;
            }
            if ((bypassFall || altFall) && m_bothTapArmed) {
                m_bothTapArmed = false;
                ev |= kBothTap;
            }
            if (!bypassDown && !altDown) {
                m_both = false;
            }
        }

        if (bypassFall) m_bypass.Release();
        if (altFall) m_alt.Release();
        return ev;
    }

  private:
    static constexpr float kFarPast = 1.0e9f;

    struct SwitchState {
        bool down = false;
        float heldMs = 0.0f;
        bool emitted = false;     // individual press event already sent
        bool consumed = false;    // part of a both-gesture; never sends individual events
        bool holdEmitted = false; // hold event already sent for this press
        void Press() {
            down = true;
            heldMs = 0.0f;
            emitted = false;
            consumed = false;
            holdEmitted = false;
        }
        void Release() {
            down = false;
            heldMs = 0.0f;
        }
    };

    Config m_cfg;
    SwitchState m_bypass;
    SwitchState m_alt;
    bool m_both = false;
    float m_bothHeldMs = 0.0f;
    bool m_bothHoldEmitted = false;
    bool m_bothTapArmed = false;
    float m_sinceLastAltRiseMs = kFarPast;
    float m_altRiseInterval = kFarPast;
    float m_lastDoubleTapIntervalMs = 0.0f;
};

} // namespace bkshepherd

#endif
```
Note on the both-hold timing: `m_bothHeldMs` starts at the second rise; the test expects both-hold at the second rise plus 2000 ms. On a release, `Release()` zeroes `heldMs` but leaves `consumed`/`emitted` as they are; `Press()` resets them, so a switch that was consumed by a both-gesture cannot leak an event on its release and behaves normally on the next press. If any test fails, fix the header, not the test, unless the test's expected time contradicts the stated timing convention; in that case explain the discrepancy in the report.

- [ ] **Step 3: Run tests, commit**

`make -C Software/GuitarPedal/tests` must pass all four suites. Commit `Add FootswitchGestures state machine with host tests`.

---

### Task 2: Wire the state machine into the firmware

**Files:**
- Modify: `Software/GuitarPedal/guitar_pedal.cpp`

**Interfaces (consumed):** `FootswitchGestures` (Task 1); existing globals `effectOn`, `isCrossFading`, `pendingEffectID`, `prevActiveEffectID`, `tunerModuleIndex`, `activeEffectID`, `availableEffectsCount`, `needToSaveSettingsForActiveEffect`, `needToChangeTempo`, `globalTempoBPM`, `guardMuteSamplesRemaining`, `guardMuteTimeInSamples`; `s_to_tempo(float seconds)`; `hardware.GetPreferredSwitchIDForSpecialFunctionType`, `hardware.switches[i].Pressed()`, `hardware.GetTimeForNumberOfSamples(size)`.

- [ ] **Step 1: Replace the switch globals**

Remove these globals and their initialization in `main()` (the `new bool[...]` / `new int[...]` block and the loop zeroing them): `ignoreBypassSwitchUntilNextActuation`, `switchEnabledIdleTimeInSeconds`, `switchEnabledIdleTimeInSamples`, `switchEnabledCache`, `switchDoubleEnabledCache`, `switchEnabledSamplesTilIdle`. Add, with the other `Util/` includes, `#include "Util/footswitch_gestures.h"`, and near the switch-monitoring section:
```cpp
// Footswitch gesture recognizer (see Util/footswitch_gestures.h). Fed once per audio block.
FootswitchGestures footswitchGestures;
const char *lastGestureName = "-"; // for the debug screen
```
In `main()`, where the removed init block was: `footswitchGestures.Init(FootswitchGestures::Config{});`.

- [ ] **Step 2: Replace the footswitch block in `AudioCallback`**

Delete everything from the comment `// Process potential footswitch actions before the main switch processing loop` through the end of the `for (int i = 0; i < hardware.GetSwitchCount(); i++) { ... }` loop (the block that ends just before `// Handle updating the Hardware Bypass & Muting signals`). Replace with:
```cpp
    // Footswitch gestures. The recognizer decides what the player meant; this block maps
    // each gesture to an action. Alt events reach the module only while the effect is on.
    {
        const int bypassId = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Bypass);
        const int altId = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate);
        const bool bypassDown = hardware.switches[bypassId].Pressed();
        const bool altDown = has_alternate_footswitch ? hardware.switches[altId].Pressed() : false;
        const float blockMs = hardware.GetTimeForNumberOfSamples(size) * 1000.0f;
        const uint32_t ev = footswitchGestures.Update(bypassDown, altDown, blockMs);

        if (ev & FootswitchGestures::kBypassTap) {
            lastGestureName = "byp tap";
            if (!isCrossFading) {
                effectOn = !effectOn;
            }
        }

        if (ev & FootswitchGestures::kBypassHold) {
            lastGestureName = "byp hold";
            if (!has_alternate_footswitch) {
                // Single-footswitch variants save on a long press.
                if (!guitarPedalUI.IsShowingSavingSettingsScreen()) {
                    needToSaveSettingsForActiveEffect = true;
                }
            } else if (hardware.SupportsDisplay() && tunerModuleIndex > 0) {
                // The tap at the start of this press already toggled effectOn. Undo that so
                // SetActiveEffect saves and restores the state the player actually had.
                effectOn = !effectOn;
                pendingEffectID = (activeEffectID == tunerModuleIndex) ? prevActiveEffectID : tunerModuleIndex;
            } else {
                // Screenless pedal: cycle to the next effect, landing bypassed.
                int next = activeEffectID + 1;
                if (next == tunerModuleIndex) next++;
                if (next > availableEffectsCount - 1) next = 0;
                effectOn = false;
                pendingEffectID = next;
            }
        }

        if (ev & FootswitchGestures::kAltPress) {
            lastGestureName = "alt press";
            if (effectOn) activeEffect->AlternateFootswitchPressed();
        }
        if (ev & FootswitchGestures::kAltDoubleTap) {
            lastGestureName = "alt dbl";
            if (activeEffect->AlternateFootswitchForTempo()) {
                needToChangeTempo = true;
                globalTempoBPM = s_to_tempo(footswitchGestures.LastDoubleTapIntervalMs() * 0.001f);
            }
        }
        if (ev & FootswitchGestures::kAltHold) {
            if (effectOn) activeEffect->AlternateFootswitchHeldFor1Second();
        }
        if (ev & FootswitchGestures::kAltRelease) {
            lastGestureName = "alt rel";
            if (effectOn) activeEffect->AlternateFootswitchReleased();
        }

        if (ev & FootswitchGestures::kBothTap) {
            lastGestureName = "both tap";
            // Jump to the next effect (Phase 4 restricts this to the active group).
            int next = activeEffectID + 1;
            if (!hardware.SupportsDisplay() && next == tunerModuleIndex) next++;
            if (next > availableEffectsCount - 1) next = 0;
            if (next == tunerModuleIndex && hardware.SupportsDisplay()) {
                // Skip the tuner when jumping; it has its own hold gesture.
                next++;
                if (next > availableEffectsCount - 1) next = 0;
            }
            pendingEffectID = next;
        }

        if (ev & FootswitchGestures::kBothHold) {
            lastGestureName = "both hold";
            if (!guitarPedalUI.IsShowingSavingSettingsScreen()) {
                needToSaveSettingsForActiveEffect = true;
            }
        }
    }
```
Keep the `// Handle updating the Hardware Bypass & Muting signals` block and everything after it unchanged. Note `pendingEffectID` is `volatile int`; the assignment from the callback is unchanged in kind from before.

- [ ] **Step 3: Mute on every effect switch**

In `SetActiveEffect`, right after the early-return guard, add:
```cpp
    // Mute briefly around the swap so the new module's first samples cannot click.
    guardMuteSamplesRemaining = guardMuteTimeInSamples;
```
(`guardMuteSamplesRemaining` is decremented in the callback; a lost decrement from the race is harmless.)

- [ ] **Step 4: Debug screen**

Replace the `sprintf(strbuff, "tap: %d", switchEnabledCache[1]);` line with `snprintf(strbuff, sizeof(strbuff), "gst %s", lastGestureName);`. Remove the `dtap` line if it still exists.

- [ ] **Step 5: Build, verify, commit**

Build 125B: no new warnings; `grep -n "switchEnabledCache\|ignoreBypassSwitchUntilNextActuation" guitar_pedal.cpp` returns nothing; DTCMRAM at or below 104100 B. Build the other four variants. Commit `Drive footswitch actions from the gesture state machine; both-tap jumps to the next effect`.

---

### Task 3: CI, release, hardware

- [ ] `make -C tests` (four suites). Push; open a PR (CI runs on PRs); download `125B-Firmware`.
- [ ] Flash Ben's pedal, then run every row of the spec section 3 table:
  - Bypass tap toggles with no perceptible delay; Alt tap on Pitch/Drop shifts; Alt double-tap on Delay sets tempo (watch BPM on the debug screen); Alt hold on TapeEcho runs away; hold Bypass 2 s enters and leaves the tuner from both on and off states; both-tap jumps to the next effect, twice in a row, and wraps at the end of the list, with no bypass toggle and no click; both-hold 2 s saves ("Saving..." screen) without jumping.
  - Debug screen `gst` shows the last gesture name; stack free above 10000; heap unchanged after ten both-taps.
- [ ] Publish a release `phase3-<date>` with `DaisyPedal-125B.bin` and the same Windows instructions as `phase2-2026-09-30`, plus a "Controls" update: both footswitches tapped together = next effect; both held 2 s = save.
