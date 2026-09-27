# Phase 1b: Stack Headroom, Crash Screen, Knob Map Font Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the crash-on-effect-switch by giving the CPU stack real headroom, make the crash report impossible to miss, make the knob map readable, and drop two unused effects.

**Architecture:** On this build the heap already lives in the 256 KB D2 SRAM bank (`end` = `0x30008000`), so the only thing in the 11132-byte gap at the top of DTCM (`__bss_end__` = `0x2001d484` to `_estack` = `0x20020000`) is the stack, shared by the main loop and every interrupt including the audio callback. newlib's allocator state (`__malloc_free_list`, `__malloc_sbrk_start`, `errno`) sits in the last 24 bytes below the stack floor, so a small overflow corrupts the allocator and the next `new`/`delete`, which every effect switch performs, faults. The fix moves the largest movable static objects out of DTCM into banks with room, removes two effects, and adds a stack high-water measurement so the margin is known rather than guessed.

**Tech Stack:** C++20, STM32H750, libDaisy v8 (sections: `DMA_BUFFER_MEM_SECTION` → RAM_D2_DMA at `0x30000000`, 32 KB, non-cacheable, live at reset; `DSY_SDRAM_BSS` → SDRAM, cacheable, usable only after `hardware.Init()`), `arm-none-eabi-gcc` 10.3, `arm-none-eabi-nm`.

Baseline (commit 224a9b6, 125B): DTCMRAM 119940 B, SRAM 414484 B, RAM_D2_DMA 16960 B of 32 KB, stack gap 11132 B.

## Global Constraints

- Only objects whose constructors do not touch peripherals may move to RAM_D2_DMA (it is live at reset, so constructors before `main` are fine). Only plain buffers with no constructor and no use before `hardware.Init()` may move to SDRAM.
- `hardware` (38 KB) and `rtneural_wavenet` (37 KB) stay in DTCM: the first drives SPI/DMA peripherals, the second is the NAM model's hot state and DTCM is the fastest memory.
- After this plan, on 125B: DTCMRAM at most 100000 B (leaves a stack gap of at least 31 KB), RAM_D2_DMA under 30 KB, SRAM under 90 percent. All five variants build.
- Every commit ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Run `clang-format -i` only on files you change; never `./ci/format.sh`. No `git stash`. No push.
- Branch `feature/next`, worktree `~/DaisySeedProjects/.worktrees/next`.

---

### Task 1: Relocate large statics out of DTCM and drop SciFi and Drum

**Files:**
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (globals `storage` line ~49, `guitarPedalUI` line ~60)
- Modify: the file that defines the 8 KB `hann` window (find it with `grep -rn "hann" Software/GuitarPedal --include=*.cpp --include=*.h | grep -v dependencies`) and `Software/GuitarPedal/Effect-Modules/spectral_delay_module.cpp` (`delay_array_real` / `delay_array_imag`, ~2.4 KB each, lines ~70-71)
- Modify: `Software/GuitarPedal/loaded_effects.h` (remove the `SciFiModule` and `DrumModule` entries and their includes), `Software/GuitarPedal/Makefile` (comment out `drum_module.cpp` and `scifi_module.cpp` in `CPP_SOURCES`, matching how the keyboard modules are commented out)

**Interfaces:**
- Produces: `storage` and `guitarPedalUI` declared with `DMA_BUFFER_MEM_SECTION` (defined in `dependencies/libDaisy/src/daisy_core.h` as `__attribute__((section(".sram1_bss")))`). `hann`, `delay_array_real`, `delay_array_imag` declared with `DSY_SDRAM_BSS` only if they are plain arrays with no constructor and are first written in an `Init`/`Process` path after `hardware.Init()`; if any of them is a `struct` array with a non-trivial constructor (`delaySpect` may be), leave that one in DTCM and say so in the report.

- [ ] **Step 1: Move `storage` and `guitarPedalUI`**

In `guitar_pedal.cpp`:
```cpp
// Persistant Storage. Lives in D2 SRAM (non-cacheable, live at reset) to keep 8 KB out of
// DTCM, which is the stack's only home. It is read a few words per audio block, which is fine.
DMA_BUFFER_MEM_SECTION PersistentStorage<Settings> storage(hardware.seed.qspi);
```
and
```cpp
// UI Related Variables. In D2 SRAM for the same reason as storage.
DMA_BUFFER_MEM_SECTION GuitarPedalUI guitarPedalUI;
```
`DMA_BUFFER_MEM_SECTION` is available through `daisy_seed.h`, which `guitar_pedal_storage.h` and the hardware headers already include.

- [ ] **Step 2: Move the spectral-delay buffers**

Where `hann` is defined, if it is `float hann[N]` (or similar plain array) with no initializer, change it to `float DSY_SDRAM_BSS hann[N];`. In `spectral_delay_module.cpp`, if `struct delaySpect` has only trivial members (check the struct near line 50-69; DaisySP `DelayLine` pointers and floats are trivial, a DaisySP filter object with a constructor is not), change lines 70-71 to `struct delaySpect DSY_SDRAM_BSS delay_array_real[delay_array_size];` and the same for `_imag`. Otherwise leave them and record it.

- [ ] **Step 3: Remove SciFi and Drum**

In `loaded_effects.h` delete the `#include "Effect-Modules/scifi_module.h"`, `#include "Effect-Modules/drum_module.h"`, `new SciFiModule(),` and `new DrumModule(), ...` lines. In `Makefile` change `CPP_SOURCES += Effect-Modules/drum_module.cpp` and `CPP_SOURCES += Effect-Modules/scifi_module.cpp` to commented lines with a note `# removed from the pedal 2026-09-27 to free memory; re-add here and in loaded_effects.h to restore`. Do not delete the source files. Note in the report that removing entries shifts effect indices, which the spec already documents as invalidating saved presets on the next format-version bump; `SETTINGS_FILE_FORMAT_VERSION` stays as is for now because the storage code already reconciles per-effect parameter counts by position and the Phase 4 groups work will bump it anyway. If the pedal boots with wrong parameter values on some effects after this, that is why, and Preset > Erase All fixes it.

- [ ] **Step 4: Build and measure**

```bash
cd ~/DaisySeedProjects/.worktrees/next/Software/GuitarPedal && make clean >/dev/null && make -j8 2>&1 | grep -E "warning|error|DTCMRAM:|SRAM:|RAM_D2" | grep -v dependencies
arm-none-eabi-nm build/guitarpedal.elf | grep -E " (__bss_end__|_estack)$"
```
Expected: DTCMRAM at most 100000 B; RAM_D2_DMA roughly 16960 + 8244 + 2736 = 27940 B (under 32 KB); `__bss_end__` at or below `0x2001869f`, so the gap to `0x20020000` is at least 31 KB. Then all five variants:
```bash
for v in 125B 1590B 1590B_SMD TERRARIUM FUNBOX; do make clean >/dev/null && make -j8 VARIANT=$v 2>&1 | grep -E "error|DTCMRAM:|RAM_D2_DMA:"; done
```

- [ ] **Step 5: Commit**

```bash
git add Software/GuitarPedal/guitar_pedal.cpp Software/GuitarPedal/loaded_effects.h Software/GuitarPedal/Makefile Software/GuitarPedal/Effect-Modules/spectral_delay_module.cpp <hann file>
git commit -m "Move storage, UI, and spectral buffers out of DTCM; drop SciFi and Drum

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Stack high-water measurement and a debug-screen toggle

**Files:**
- Create: `Software/GuitarPedal/Util/stack_guard.h`
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (`main` start; debug display; encoder hold toggle)

**Interfaces:**
- Produces:
  - `void bkshepherd::StackPaint()` fills the unused stack from `__bss_end__` up to just below the current stack pointer with `0xA5`. Call it as the very first statement of `main()`.
  - `uint32_t bkshepherd::StackFreeBytes()` scans upward from `__bss_end__` for the first byte that is not `0xA5` and returns how many painted bytes remain, which is the minimum free stack ever observed.
  - `uint32_t bkshepherd::StackTotalBytes()` returns `_estack - __bss_end__`.
  - Holding the encoder button for 3 seconds toggles `useDebugDisplay`; the debug screen shows `stk <free>/<total>` and the guard trip counter.

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#ifndef STACK_GUARD_H
#define STACK_GUARD_H

#include <cstdint>

// Stack high-water measurement. The stack is the only thing in the gap between the end
// of .bss and _estack in DTCM; overflowing it corrupts newlib's allocator state, which
// sits directly below the stack floor. Paint the free part at boot, then look for the
// highest unpainted byte later.

extern "C" char __bss_end__; // provided by the linker script
extern "C" char _estack;

namespace bkshepherd {

constexpr uint8_t kStackPaint = 0xA5;

inline uint32_t StackTotalBytes() {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_estack) - reinterpret_cast<uintptr_t>(&__bss_end__));
}

/** Call first thing in main(). Paints from the stack floor up to 64 bytes below the
 *  current stack pointer so the frame we are running in is left alone. */
inline void StackPaint() {
    uint8_t *floor = reinterpret_cast<uint8_t *>(&__bss_end__);
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
    const uint8_t *floor = reinterpret_cast<const uint8_t *>(&__bss_end__);
    const uint8_t *top = reinterpret_cast<const uint8_t *>(&_estack);
    uint32_t n = 0;
    for (const uint8_t *p = floor; p < top && *p == kStackPaint; ++p) {
        ++n;
    }
    return n;
}

} // namespace bkshepherd

#endif
```

- [ ] **Step 2: Paint at boot and expose the numbers**

In `guitar_pedal.cpp`: include `"Util/stack_guard.h"`; make `StackPaint();` the first statement in `main()` (before `hardware.Init`). Change `bool useDebugDisplay = false;` to stay `false` but add, in the main loop near the encoder handling:
```cpp
        // Hold the encoder button 3 s to toggle the debug screen (stack, guard trips, tempo).
        {
            static bool debugToggleArmed = true;
            const float heldMs = hardware.encoders[0].TimeHeldMs();
            if (heldMs > 3000.0f && debugToggleArmed) {
                useDebugDisplay = !useDebugDisplay;
                debugToggleArmed = false;
            }
            if (heldMs <= 0.0f) {
                debugToggleArmed = true;
            }
        }
```
In the debug display block add a line at y=57 (or replace the `dtap` line, which is not useful) showing `stk %lu/%lu` with `StackFreeBytes()` and `StackTotalBytes()` cast to `unsigned long`. Keep `grd`.

- [ ] **Step 3: Build, commit**

Build 125B; no new warnings; DTCMRAM unchanged from Task 1. Commit `Add stack high-water measurement and encoder-hold debug screen toggle`.

---

### Task 3: Crash screen waits for a footswitch; knob map in the menu font

**Files:**
- Modify: `Software/GuitarPedal/guitar_pedal.cpp` (crash report block in `main`)
- Modify: `Software/GuitarPedal/Effect-Modules/base_effect_module.cpp` (`DrawKnobMap`)

- [ ] **Step 1: Crash screen persists**

In the crash report block, after drawing the screen and doing the five blinks, replace the fall-through with a wait: loop calling `hardware.ProcessDigitalControls()` and `System::Delay(10)` until either footswitch reports `RisingEdge()` or 60 seconds pass (a 60 s timeout so an unattended pedal still starts). Add a fifth screen line at y=56 in `Font_6x8` or `Font_7x10` if it fits: `tap a footswitch`. The watchdog is not running yet at this point, so the wait cannot trigger it. Keep clearing the record afterward.

- [ ] **Step 2: Knob map labels in Font_7x10 with the effect-name prefix stripped**

In `DrawKnobMap`: change the label font to `Font_7x10` and `maxLabelChars` to 6 (6 × 7 = 42 px, exactly one cell; the cell width is `boundsToDrawIn.GetWidth() / 3` = 42 on 128 px). Before truncating, if the parameter name begins with the effect's `m_name` followed by a space (compare case-insensitively, e.g. "Delay Time" on effect "Delay"), skip that prefix; also skip a leading "D " when the effect name starts with "D" (so "D Feedback" → "Feedba"). Implement as a small static helper `const char *StripEffectPrefix(const char *effectName, const char *paramName)` in the same file. Row height stays `(52 - 2) / 2` = 25 px, which fits the 10 px glyphs. The title row keeps `Font_7x10`.

- [ ] **Step 3: Build all variants, commit**

Expected on 125B: DTCMRAM unchanged, SRAM may drop a little because `Font_5x8` is no longer referenced. Commit `Crash screen waits for a footswitch; knob map uses the menu font`.

---

### Task 4: CI, artifact, hardware

- [ ] Run `make -C tests`, push, watch the PR's CI run, download `125B-Firmware`.
- [ ] Flash Ben's pedal. Then: boot; hold the encoder 3 s to open the debug screen and read `stk free/total` after (a) idle, (b) switching effects with Alt + encoder through the whole list twice, (c) 30 s of Delay squeal, (d) opening every menu. Record the smallest free value. Expect free never below 8 KB with total about 31 KB or more.
- [ ] Reproduce the original crash procedure (spin the encoder through effects with Alt held) for two minutes. Expect no reboot. If it does reboot, the crash screen now stays until a footswitch tap; write down pc, cfsr, fx.
- [ ] Knob map on Delay: labels readable, first row "Time", "Feedba", "Mix" (or per the stripping rule).
