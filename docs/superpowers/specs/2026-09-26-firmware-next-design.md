# GuitarPedal firmware: bug fixes, footswitch gestures, effect groups, new modules, docs

Date: 2026-09-26
Branch: `feature/next` (from `main` at 5f5edda)
Hardware target: 125B variant (Ben's and Steve's pedals). On this build the right footswitch is Bypass and the left is Alt. All changes must still build for the other four variants in CI.

## 1. Goals

1. Fix three field bugs: tuner receives no audio, LEDs go dark after switching effects with the encoder or menu, and hard crashes under loud input or feedback.
2. Make the alternate footswitch useful on more effects: tap tempo on the modulation and delay effects, IR cycling on the IR module.
3. Add a two-knob Fender-style tremolo, a dedicated Drop pitch module with a momentary mode, and a Space Echo style tape echo.
4. Add "effect groups": named subsets of the loaded effects that can be cycled with both footswitches, so a player can lock a set for a gig.
5. Ship a user manual and step-by-step flashing guides for macOS and Windows in the repo.

Out of scope: running two effects at once, improving the pitch-shift algorithm, any ESP32 work.

## 2. Bug fixes

### 2.1 Tuner receives no audio

Cause (confirmed in `guitar_pedal.cpp`): the audio callback only calls the active effect when `effectOn` is true or a crossfade is in progress. The hold-Bypass quick switch forces `effectOn = true`, but selecting Tuner from the Settings menu, with Alt + encoder, or via MIDI program change leaves `effectOn` as it was. On a bypassed pedal the tuner never gets a sample and shows an empty meter.

Fix: in `SetActiveEffect`, when the new effect is the tuner, remember `effectOn`, force it true, and enable the module. When leaving the tuner by any route, restore the remembered state. This replaces the ad hoc `effectActiveBeforeQuickSwitch` handling with one path.

Verification on hardware: reach the tuner three ways (hold Bypass, Settings menu, Alt + encoder) from both a bypassed and an engaged state. A played E must show "E" and a centered meter each time.

Follow-up if the meter is still blank after the fix: the tuner reads only the left input, and the Q pitch detector needs a healthy signal level. Check which jack the guitar is in and log the detected frequency over USB serial.

### 2.2 LEDs dark after switching effects

Cause (confirmed): `BaseEffectModule::GetBrightnessForLED` returns 0 unless the module's own `m_isEnabled` is true. `SetEnabled` is called on the bypass toggle, in the tuner quick switch, and once at startup, but not in `SetActiveEffect`. A module reached through the menu or Alt + encoder has never been enabled, so LED 0 stays off until the player toggles bypass twice. Some modules also gate processing on this flag.

Fix: `SetActiveEffect` calls `SetEnabled(false)` on the outgoing module and `SetEnabled(effectOn)` on the incoming one. Remove the now redundant calls at the quick-switch call sites.

### 2.3 Crashes under loud input, feedback, or switching

Root cause is not confirmed. Two mechanisms are consistent with the report:

- A feedback path (delay, reverb, CloudSeed) driven hard produces `inf`, then `NaN`. Several modules derive buffer indices from float state. A `NaN` cast to an integer index is undefined and can read or write outside a delay buffer, which is a hard fault on this MCU. The pedal then sits silent with LEDs frozen until power cycle.
- Switching effects while the UI rebuilds its menu arrays is a main-loop-only operation and should be safe. It stays on the suspect list only if the guard below does not stop the crashes.

Mitigations, all three:

1. **Output sanitizer** in the audio callback: after the active effect processes a sample, if either output is not finite, replace it with 0 and count the event. If a block ends with any non-finite output, re-`Init` the active effect on the next main-loop pass and mute for 20 ms. This bounds the damage to a short dropout.
2. **Input clamp**: clamp codec input to [-1, 1] before processing. Costs nothing and removes one class of overflow.
3. **Independent watchdog** (IWDG, 2 s) kicked from the main loop. A hard fault or hang becomes a 2-second reboot instead of a frozen pedal. libDaisy exposes this directly.

Steve's recollection is that a feedback or pitch-shift effect such as the Delay was active. That matches the first mechanism, so the crash-guard test in section 8 targets Delay at maximum feedback and Pitch at an octave.

Diagnostics for the next crash: enable the hard-fault handler to flash both LEDs in a distinctive pattern and record the faulting address in a no-init RAM word that is printed over USB serial at next boot.

## 3. Footswitch gesture state machine

Today the callback recognizes gestures with scattered timers and flags (`switchEnabledCache`, `ignoreBypassSwitchUntilNextActuation`, and so on). Replace them with one small class, `FootswitchGestures`, fed by the two debounced switches each block, that emits at most one event per block:

| Event | Detection | Action |
|---|---|---|
| Bypass tap | Bypass pressed and released, or held past 80 ms, with Alt not pressed within that window | Toggle effect on/off |
| Bypass hold | Bypass held 2 s, Alt up | Quick switch to/from tuner (unchanged) |
| Alt tap | Alt pressed, Bypass not pressed within 80 ms | `AlternateFootswitchPressed` / `Released` on the module |
| Alt double tap | Two Alt taps inside 2 s | Tap tempo, if the module accepts tempo (unchanged) |
| Alt hold | Alt held 1 s | `AlternateFootswitchHeldFor1Second` (unchanged) |
| Both tap | Both down within 80 ms of each other, released before 2 s | Next effect in the active group (section 5). If group mode is off, next effect in the full list. |
| Both hold | Both held 2 s | Save current preset (unchanged) |

Trade-off: the bypass toggle now fires up to 80 ms after the press instead of instantly, to disambiguate from "both tap". Not perceptible at the footswitch.

On Bypass tap the module's `AlternateFootswitchPressed` must not fire, and vice versa, which the window guarantees.

Only variants with two footswitches use "both" events. Single-footswitch variants keep the current behavior.

Effect switching by any route mutes the output for 20 ms around the swap to avoid clicks.

## 4. Alternate footswitch actions

Current behavior is documented in the manual (section 7). Changes:

| Effect | New Alt behavior |
|---|---|
| Chorus | Double tap sets rate from tempo (`SetTempo` override, one cycle per beat) |
| Phaser | Same |
| Flanger | Same |
| AmpTrem (new, section 6) | Same |
| IR | Tap cycles to the next impulse response, wrapping. Re-enable the commented code in `ir_module.cpp` with the signed-index fix. LED 1 blinks once on each step. |
| Drop (new) | Hold shifts while held, in Moment mode |
| TapeEcho (new) | Double tap sets Rate; hold pushes Repeats to maximum while held |

Delay, Multi Delay, Tremolo, AutoPan, Chopper, Metronome, and Drum already implement tap tempo. Nothing else changes.

## 5. Effect groups

### 5.1 Behavior

- Up to 4 groups. Each group is an ordered subset of the loaded effects. Order is the order in `loaded_effects.h`; no custom ordering in this version.
- A global setting `Group mode` is Off or Group 1 to 4.
- When a group is active, Both tap advances to the next effect in that group and wraps. Alt + encoder also steps within the group. The Settings > Effect list remains unrestricted as the escape hatch, and the tuner quick switch is unaffected.
- Each effect loads with its currently selected preset and its own saved parameters. Knobs do not write to the new effect until moved past the existing tolerance. On every effect switch the per-knob "changed" flags are cleared so a knob that was moving in the previous second cannot leak into the new effect.
- An empty group behaves as Group mode Off.

### 5.2 Storage

Add to `Settings`:

```
uint64_t groupMasks[4];   // bit i set = effect i is in the group
uint8_t  activeGroup;     // 0 = off, 1..4
```

Bump `SETTINGS_FILE_FORMAT_VERSION` to 9. This factory-resets stored presets on first boot after the update, which Ben accepted. `Settings` lives in DTCM, which is at 91 percent, so the addition is 33 bytes and nothing else may be added there.

Effects are identified by list index. Reordering `loaded_effects.h` in a later build will scramble group membership. Document this in the manual; a name-based mapping is a possible later improvement.

### 5.3 UI

New main-menu page "Groups":

- `Mode`: Off / Group 1 / Group 2 / Group 3 / Group 4
- `Edit Group 1` to `Edit Group 4`: opens a checklist of every loaded effect. Toggling a checkbox updates the mask immediately. The tuner is excluded from the checklist.
- `Back`

The OLED effect page shows `G1 2/4` style position when a group is active.

## 6. New effect modules

### 6.1 AmpTrem

A two-knob amplifier-style tremolo.

- Parameters: `Speed` (knob 0, 1 to 12 Hz, log curve), `Intensity` (knob 1, 0 to 1). MIDI CC on both.
- Waveform: sine with a soft-clipped shape so full intensity has a slightly flattened top, approximating an optical tremolo. No secondary LFO.
- Tap tempo sets Speed.
- LED 1 follows the LFO, same as the existing Tremolo.
- Implementation: copy of `modulated_tremolo_module` with the modulation oscillator removed. Under 150 lines, a few KB of SRAM.

### 6.2 Drop

A dedicated pitch-down module, so the general Pitch module's six knobs are not needed for the common case. Always shifts down.

- Parameters: `Semitones` (knob 0, binned 1 to 12, default 2), `Mix` (knob 1, default 1.0 wet), `Mode` (knob 2, binned Latch / Moment, default Latch).
- Latch: shifted whenever the effect is engaged.
- Moment: shifted only while the Alt footswitch is held, returning on release, with a fixed 100 ms ramp each way. No ramp knobs; the general Pitch module keeps those.
- Delay size scales with the interval as the existing `SetTranspose` does. Latency is roughly 42 ms at one semitone and grows with the interval; document this.
- LED 1 lit while the shift is active, so Moment mode is visible.
- Reuses `Util/pitch_shifter.h` and its own pair of SDRAM buffers. SDRAM has ample room.
- The general Pitch module stays in the build and is already in the loaded list as "Pitch" (it was absent from the December 2024 firmware, which is why it was never found). Removing it later is a one-line change in `loaded_effects.h`.

### 6.3 TapeEcho

A tape echo modeled on the Roland RE-201 Space Echo control set, with the Akai Headrush E2's adjustable wow and flutter. Reuses the delay engine and the tape modulator that the Delay module gained in January 2026, and adds the two things a tape echo has that the Delay does not: saturation in the feedback loop and multiple playback heads.

- Parameters: `Rate` (knob 0, delay time 40 to 800 ms, tap tempo sets it), `Repeats` (knob 1, feedback 0 to 1.1, so it can self-oscillate like the original but is limited by the saturator), `Echo Vol` (knob 2, wet mix), `Wow Flut` (knob 3, wow and flutter depth, 0 = clean digital), `Tone` (knob 4, low-pass in the feedback path, so each repeat gets darker), `Heads` (knob 5, binned: 1, 2, 3, 1+2, 2+3, 1+2+3, 1+3). Head spacing follows the RE-201: head 2 at 2x and head 3 at 3x the head 1 time.
- Feedback loop: soft-clip saturation before the filter, so runaway repeats compress instead of clipping hard. This also keeps the loop finite, which matters for section 2.3.
- Alt footswitch: double tap sets Rate from tempo. Alt hold: repeats to maximum while held, for the Space Echo self-oscillation trick, returning on release.
- LED 1 pulses at the delay rate.
- Stereo: same echo on both channels. Spread and ping-pong omitted in this version.
- Delay buffers in SDRAM, three read taps. CPU cost is close to the existing Delay module.
- Spring reverb from the RE-201 is out of scope; the Reverb module exists for that.

## 7. Documentation deliverables

All under `docs/` in the repo and linked from the top-level README.

1. `docs/MANUAL.md`: user manual for the 125B pedal. Controls, power-up behavior, bypass and LEDs, the effect list with each effect's knobs, a table of what the Bypass and Alt footswitches do per effect, presets (save, select, erase), effect groups, tuner, MIDI, global settings, and known limitations (single effect at a time, pitch latency, preset wipe on format change).
2. `docs/FLASHING-MAC.md` and `docs/FLASHING-WINDOWS.md`: numbered step-by-step guides. Both cover the recommended path first: download the variant's `.bin` from the GitHub Actions "Build All" run, open the Daisy web flasher in Chrome, press RESET then BOOT within 5 seconds to lock the bootloader, upload, flash. Then the command-line path with `dfu-util`: Homebrew install on macOS; on Windows, the dfu-util zip plus the WinUSB driver via Zadig, which is the step that trips most people. Both guides include first-time bootloader installation and a troubleshooting section covering the two failure modes seen on 2026-09-23: the pedal appearing in the ROM bootloader ("Internal Flash", QSPI address rejected) because BOOT was held during RESET, and the pedal dropping off the bus because BOOT was not pressed after RESET.

## 8. Testing

There is no host build, so testing is on hardware plus CI.

- CI "Build All" must pass for all five variants, and the 125B memory report must stay under 95 percent DTCM and 90 percent SRAM.
- Hardware checklist, run on Ben's pedal before flashing Steve's:
  - Tuner: three entry routes from both bypass states.
  - LEDs: switch effects via encoder and menu while engaged; LED 0 stays lit.
  - Gestures: each row of the section 3 table, including that a Bypass tap never triggers the module's Alt action.
  - Crash guard: run the Delay with feedback at maximum into a squeal for 30 seconds, then Pitch at an octave with the guitar feeding back into the amp. Expect at worst a brief dropout, never a frozen pedal.
  - Groups: build a group of four, cycle it with Both tap, confirm wrap, confirm knobs do not jump on switch, power cycle and confirm the group survives.
  - AmpTrem, Drop, TapeEcho: knobs, tap tempo on AmpTrem and TapeEcho, Drop Moment mode on Alt hold, TapeEcho self-oscillation on Alt hold, LED behavior.
  - IR: Alt cycles and wraps.

## 9. Sequence

1. Bug fixes (2.1, 2.2, 2.3) and the flashing guides. Flash Steve's pedal with this build first.
2. AmpTrem, Drop, and TapeEcho modules.
3. Footswitch gesture state machine and Both tap next-effect.
4. Effect groups and the Groups UI.
5. Alt actions on Chorus, Phaser, Flanger, IR.
6. Manual, written last so it describes the shipped behavior.

## 10. Resolved questions

1. Bypass is the right footswitch and Alt is the left on Ben's and Steve's 125B builds. The manual uses those words.
2. The effect active at the crashes is not known. Steve believes it was a feedback or pitch-shift effect such as the Delay. Section 2.3 covers this.
