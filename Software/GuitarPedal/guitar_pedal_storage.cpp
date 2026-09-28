#include "guitar_pedal_storage.h"
#include "Effect-Modules/base_effect_module.h"
#include "Util/watchdog.h"

using namespace bkshepherd;

extern PersistentStorage<Settings> storage;
extern int availableEffectsCount;
extern BaseEffectModule **availableEffects;
extern int activeEffectID;
extern BaseEffectModule *activeEffect;

uint32_t GetDefaultTotalIdxOfGlobalSettingsBlock() {
    uint32_t tempSize = 0;

    for (int effectID = 0; effectID < availableEffectsCount; effectID++) {
        int paramCount = availableEffects[effectID]->GetParameterCount();
        // Reserve 2 words for Num of Presets and Num of Parameters
        tempSize += 2 + paramCount;
    }
    return tempSize;
}

// FNV-1a 32-bit fingerprint of the compiled-in effect list: seeded with availableEffectsCount,
// then for each effect in order, every byte of its name, a 0 separator byte, and the low byte of
// its parameter count. Two firmwares with the same effects in the same order and the same
// parameter counts produce the same fingerprint; adding, removing, or reordering an effect (or
// changing one's parameter count) changes it. Used by InitPersistantStorage() to detect a stored
// settings block that was written for a different effect list than the one currently compiled in.
uint32_t ComputeEffectListFingerprint() {
    uint32_t hash = 2166136261U;
    const uint32_t prime = 16777619U;

    auto mixByte = [&hash, prime](uint8_t b) {
        hash ^= b;
        hash *= prime;
    };

    uint32_t effectsCount = static_cast<uint32_t>(availableEffectsCount);
    for (size_t i = 0; i < sizeof(effectsCount); i++) {
        mixByte(static_cast<uint8_t>((effectsCount >> (8 * i)) & 0xFFU));
    }

    for (int effectID = 0; effectID < availableEffectsCount; effectID++) {
        const char *name = availableEffects[effectID]->GetName();
        for (const char *c = name; *c != '\0'; ++c) {
            mixByte(static_cast<uint8_t>(*c));
        }
        mixByte(0U);
        mixByte(static_cast<uint8_t>(availableEffects[effectID]->GetParameterCount() & 0xFFU));
    }

    return hash;
}
void InitPersistantStorage() {
    Settings defaultSettings;
    defaultSettings.fileFormatVersion = SETTINGS_FILE_FORMAT_VERSION;
    defaultSettings.globalActiveEffectID = 0;
    defaultSettings.globalMidiEnabled = true;
    defaultSettings.globalMidiChannel = 1;
    defaultSettings.globalMidiThrough = true;
    defaultSettings.globalRelayBypassEnabled = false;
    defaultSettings.globalSplitMonoInputToStereo = true;
    defaultSettings.globalEffectListFingerprint = ComputeEffectListFingerprint();

    // All Effect Params in the settings should be zero'd
    for (int i = 0; i < SETTINGS_ABSOLUTE_MAX_PARAM_COUNT; i++) {
        defaultSettings.globalEffectsSettings[i] = 0;
    }

    uint32_t globalEffectsSettingMemIdx = 0U;
    defaultSettings.globalEffectsSettings[globalEffectsSettingMemIdx] = GetDefaultTotalIdxOfGlobalSettingsBlock();
    ++globalEffectsSettingMemIdx;

    // Override any defaults with effect specific default settings
    for (int effectID = 0; effectID < availableEffectsCount; effectID++) {
        int paramCount = availableEffects[effectID]->GetParameterCount();
        // Change first word of each effect such that this is total number of presets
        // Default value: 1
        defaultSettings.globalEffectsSettings[globalEffectsSettingMemIdx] = 1U;
        ++globalEffectsSettingMemIdx;

        // Next word is going to be the number of parameters
        defaultSettings.globalEffectsSettings[globalEffectsSettingMemIdx] = paramCount;
        ++globalEffectsSettingMemIdx;

        for (int paramID = 0; paramID < paramCount; paramID++) {
            // Floats are handled special (stored into the u_int32_t bytes)
            if (availableEffects[effectID]->GetParameterType(paramID) == ParameterValueType::Float) {
                uint32_t tmp;
                float f = availableEffects[effectID]->GetParameterAsFloat(paramID);
                std::memcpy(&tmp, &f, sizeof(float));
                defaultSettings.globalEffectsSettings[globalEffectsSettingMemIdx] = tmp;
            } else {
                defaultSettings.globalEffectsSettings[globalEffectsSettingMemIdx] =
                    availableEffects[effectID]->GetParameterRaw(paramID);
            }

            ++globalEffectsSettingMemIdx;
        }
    }

    storage.Init(defaultSettings);

    Settings &settings = storage.GetSettings();

    // If the stored data is not the current version, or was written for a different effect list
    // (an effect added/removed/reordered, or a parameter count changed), do a factory reset.
    // Without the fingerprint check, a stored block from a different effect list would still
    // pass the version check (if the file format itself didn't change) and then feed stale
    // per-effect preset/parameter counts into LoadEffectSettingsFromPersistantStorage(), which
    // is exactly what caused the 2026-09-28 boot loop after SciFi/Drum were removed.
    if (settings.fileFormatVersion != SETTINGS_FILE_FORMAT_VERSION ||
        settings.globalEffectListFingerprint != ComputeEffectListFingerprint()) {
        storage.RestoreDefaults();
    }

    // Make sure the settings active effect is within the proper range.
    if (settings.globalActiveEffectID < 0 || settings.globalActiveEffectID >= availableEffectsCount) {
        settings.globalActiveEffectID = 0;
    }
}

uint32_t ShiftSettingsToMatchCurrentParameters(uint32_t prev_params, uint32_t curr_params, uint32_t effectID, uint32_t presetsCount,
                                               uint32_t shiftStartIdx, uint32_t currentMaxIdx) {
    uint32_t newMaxIdx = currentMaxIdx;
    Settings &settings = storage.GetSettings();

    newMaxIdx += presetsCount * (curr_params - prev_params);
    // Valid indices into globalEffectsSettings are [0, SETTINGS_ABSOLUTE_MAX_PARAM_COUNT); newMaxIdx
    // is used below as an inclusive last index, so it must be strictly less than the array size, not
    // merely <=, or the loops just below would write/read globalEffectsSettings[SETTINGS_ABSOLUTE_MAX_PARAM_COUNT],
    // one past the end.
    if (newMaxIdx < SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
        if (curr_params > prev_params) {
            uint32_t diff = newMaxIdx - currentMaxIdx;
            for (uint32_t i = newMaxIdx; i >= shiftStartIdx; --i) {
                settings.globalEffectsSettings[i] = settings.globalEffectsSettings[i - diff];
            }
        } else {
            uint32_t diff = currentMaxIdx - newMaxIdx;
            for (uint32_t i = shiftStartIdx; i < newMaxIdx; ++i) {
                settings.globalEffectsSettings[i] = settings.globalEffectsSettings[i + diff];
            }
            // Clear words up until the last maximum
            for (uint32_t i = newMaxIdx; i <= currentMaxIdx; ++i) {
                settings.globalEffectsSettings[i] = 0;
            }
        }
    } else {
        newMaxIdx = ERR_VALUE_MAX;
    }

    return newMaxIdx;
}

uint32_t ShiftSettingsToAddNewPreset(int effectID, uint32_t params, uint32_t shiftStartIdx, uint32_t currentMaxIdx) {
    uint32_t newMaxIdx = currentMaxIdx;
    Settings &settings = storage.GetSettings();

    newMaxIdx += params;
    // See the identical comment in ShiftSettingsToMatchCurrentParameters(): newMaxIdx is an inclusive
    // last index, so it must be strictly less than the array size.
    if (newMaxIdx < SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
        uint32_t diff = newMaxIdx - currentMaxIdx;
        for (uint32_t i = newMaxIdx; i >= shiftStartIdx; --i) {
            settings.globalEffectsSettings[i] = settings.globalEffectsSettings[i - diff];
        }
        for (uint32_t i = effectID + 1; i < (uint32_t)availableEffectsCount; ++i) {
            uint32_t tmp = availableEffects[i]->GetSettingsArrayStartIdx() + diff;
            availableEffects[i]->SetSettingsArrayStartIdx(tmp);
        }
    } else {
        newMaxIdx = ERR_VALUE_MAX;
    }

    return newMaxIdx;
}

void LoadPresetFromPersistentStorage(uint32_t effectID, uint32_t presetID) {
    uint32_t presetCount = availableEffects[effectID]->GetPresetCount();
    // Get a handle to the persitance storage settings
    Settings &settings = storage.GetSettings();
    if (effectID >= 0 && effectID < (uint32_t)availableEffectsCount && presetID < presetCount) {
        int paramCount = availableEffects[effectID]->GetParameterCount();
        uint32_t startIdx;

        startIdx = availableEffects[effectID]->GetSettingsArrayStartIdx() + 2 + (paramCount * presetID);

        for (int paramID = 0; paramID < paramCount; paramID++) {
            if (availableEffects[effectID]->GetParameterType(paramID) == ParameterValueType::Float) {
                uint32_t tmp = settings.globalEffectsSettings[startIdx + paramID];
                float f;
                std::memcpy(&f, &tmp, sizeof(float));
                availableEffects[effectID]->SetParameterAsFloat(paramID, f);
            } else {
                availableEffects[effectID]->SetParameterRaw(paramID, settings.globalEffectsSettings[startIdx + paramID]);
            }
        }
    }
}

// Settings-table integrity guard for LoadEffectSettingsFromPersistantStorage().
//
// The stored table starting at settings.globalEffectsSettings[0] has no independent length or
// checksum: the loader below discovers where each effect's data lives by walking forward,
// trusting the stored `presetsCount`/`prevParamCount` words it reads along the way to know how
// far to advance for each effect. This function performs that same walk using only reads --
// it never mutates `settings` or any effect module -- and reports whether every index the real
// loader would touch stays inside `globalEffectsSettings` (size SETTINGS_ABSOLUTE_MAX_PARAM_COUNT).
// It returns false on the first violation it finds.
//
// This function requires `prevParamCount == paramCount` for every effect. That's not just a
// simplification: InitPersistantStorage() already factory-resets on a
// globalEffectListFingerprint mismatch, and the fingerprint is computed from each effect's name
// *and parameter count*, so any table that reaches here with a matching fingerprint is
// guaranteed to have been written by this exact compiled-in effect list -- `prevParamCount` can
// only equal `paramCount`. A stored table where they differ is therefore never legitimate (it's
// either corrupt or from a firmware build this fingerprint scheme can't have produced), so it is
// rejected here rather than walked through the loader's grow/shrink branches, which only exist
// to keep this validator's job simple: with parameter counts always matching, every effect's
// span is just `paramCount * presetsCount` words (the presetsCount-1 additional presets, plus
// preset 0), with no shifting to reason about.
//
// Finally, the walk's own end position is cross-checked against the stored word 0: `word 0 ==
// GetDefaultTotalIdxOfGlobalSettingsBlock()`-style total-index-word convention. That helper (see
// above) stores, for the compiled-in defaults (presetsCount == 1 for every effect),
// `sum(2 + paramCount_i)` in word 0 -- i.e. the *highest occupied index* in the table, since the
// body starts at index 1 and is that many words long. SaveEffectSettingsToPersitantStorageForEffectID()
// keeps that invariant as presets are added. So after this walk finishes at `globalEffectsSettingMemIdx`
// (one past the last word it covered), the highest occupied index is `globalEffectsSettingMemIdx - 1`,
// and word 0 must equal exactly that.
static bool SettingsLayoutLooksValid(const Settings &settings) {
    uint32_t globalEffectsSettingMemIdx = 0U;

    if (settings.globalEffectsSettings[globalEffectsSettingMemIdx] > SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
        return false;
    }
    ++globalEffectsSettingMemIdx;

    for (int effectID = 0; effectID < availableEffectsCount; effectID++) {
        if (globalEffectsSettingMemIdx >= SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            return false;
        }
        uint32_t presetsCount = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
        if (presetsCount < 1U || presetsCount > kMaxPresetsPerEffect) {
            return false;
        }
        ++globalEffectsSettingMemIdx;

        if (globalEffectsSettingMemIdx >= SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            return false;
        }
        uint32_t prevParamCount = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
        ++globalEffectsSettingMemIdx;

        // See the big comment above: with a matching fingerprint, prevParamCount can only be
        // legitimate if it equals the compiled-in paramCount. This also makes a standalone
        // "is prevParamCount a plausible small number" bound unnecessary -- paramCount itself is
        // always a small, trusted (compiled-in) value, so the equality check below is a tighter
        // bound than any arbitrary cap on the raw stored word could be.
        uint32_t paramCount = availableEffects[effectID]->GetParameterCount();
        if (prevParamCount != paramCount) {
            return false;
        }

        uint64_t span = static_cast<uint64_t>(paramCount) * static_cast<uint64_t>(presetsCount);
        if (static_cast<uint64_t>(globalEffectsSettingMemIdx) + span > static_cast<uint64_t>(SETTINGS_ABSOLUTE_MAX_PARAM_COUNT)) {
            return false;
        }
        globalEffectsSettingMemIdx += static_cast<uint32_t>(span);
    }

    // Word 0 must equal the highest index this walk actually occupied (see the comment above).
    if (globalEffectsSettingMemIdx == 0U || settings.globalEffectsSettings[0] != globalEffectsSettingMemIdx - 1U) {
        return false;
    }

    return true;
}

// Loads Preset 0 of every effect's parameters from Persistent Storage into the effect modules.
//
// Invariants / recovery path (added 2026-09-28 after a boot loop on hardware: a settings block
// written by a 27-effect firmware was loaded by a 25-effect firmware whose
// SETTINGS_FILE_FORMAT_VERSION had not changed, so no factory reset ran; this loop trusted the
// stale stored presetsCount/prevParamCount words to advance its index and walked far outside
// globalEffectsSettings, producing a precise bus fault a few seconds into every boot):
//   1. InitPersistantStorage() already resets to defaults on a fileFormatVersion mismatch *or* a
//      globalEffectListFingerprint mismatch (effect list/order/param-count changed), so in the
//      common case the table below was written by this exact firmware and matches its layout.
//   2. Even so, this function never trusts that on faith: SettingsLayoutLooksValid() re-walks the
//      table first. If it fails, storage.RestoreDefaults() is tried once and the table is
//      re-checked; if it's still invalid, this function returns without loading anything, and
//      every effect module simply keeps the compiled-in defaults it already has from Init().
//      SettingsLayoutLooksValid() also requires prevParamCount == paramCount for every effect
//      (see its own comment), so on the boot path below, the `prevParamCount < paramCount` and
//      `prevParamCount > paramCount` branches -- and therefore every call this function makes
//      into ShiftSettingsToMatchCurrentParameters(), which exists solely to serve those two
//      branches -- are unreachable: only the "counts match" branch can run. Those branches and
//      calls are left in place rather than deleted, as belt-and-suspenders in case that
//      invariant is ever weakened (e.g. by a future change to the fingerprint or the
//      validator), and because they carry their own inline bounds checks (see point 3 below)
//      that keep this function safe on its own even if that invariant were to stop holding.
//   3. The loop below additionally re-checks its own bounds inline (breaking out of the loop on
//      any violation) so that a table that satisfied SettingsLayoutLooksValid() up front, but
//      would be walked differently by the loop for any reason, still cannot index outside
//      globalEffectsSettings. No index here is ever used unchecked.
// The net effect: this function must never fault, regardless of what garbage flash may contain.
void LoadEffectSettingsFromPersistantStorage() {
    Settings &settings = storage.GetSettings();

    if (!SettingsLayoutLooksValid(settings)) {
        storage.RestoreDefaults();
        if (!SettingsLayoutLooksValid(settings)) {
            // Give up quietly; every effect module keeps the compiled defaults set in Init().
            return;
        }
    }

    uint32_t globalEffectsSettingMemIdx = 0U;
    uint32_t globalEffectsMaxIdx = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
    ++globalEffectsSettingMemIdx;
    // Load Preset 0 of each Effect Parameters, based on values from Persistant Storage
    for (int effectID = 0; effectID < availableEffectsCount; effectID++) {
        if (globalEffectsSettingMemIdx >= SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            break;
        }
        uint32_t presetsCount = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
        if (presetsCount < 1U || presetsCount > kMaxPresetsPerEffect) {
            break;
        }
        availableEffects[effectID]->SetSettingsArrayStartIdx(globalEffectsSettingMemIdx);
        ++globalEffectsSettingMemIdx;

        if (globalEffectsSettingMemIdx >= SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            break;
        }
        uint32_t paramCount = availableEffects[effectID]->GetParameterCount();
        uint32_t prevParamCount = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
        ++globalEffectsSettingMemIdx;

        // SettingsLayoutLooksValid() already required prevParamCount == paramCount for every
        // effect before this loop was allowed to run (see the comment above), so this can only
        // fail if the table changed under us between that check and here; re-checking it locally
        // means the grow/shrink branches below are unreachable, not just "assumed unreachable".
        if (prevParamCount != paramCount) {
            break;
        }

        // Every branch below reads/writes at most `paramCount` words starting here; bound that
        // whole span up front so none of the three branches can read or write out of range.
        if (static_cast<uint64_t>(globalEffectsSettingMemIdx) + paramCount > SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            break;
        }

        // If the two variables are the same, there's no problem, just load value from the global settings copy
        if (paramCount == prevParamCount) {
            for (uint32_t paramID = 0; paramID < paramCount; paramID++) {
                uint32_t value = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
                if (availableEffects[effectID]->GetParameterType(paramID) == ParameterValueType::Float) {
                    float tmp;
                    std::memcpy(&tmp, &value, sizeof(float));
                    availableEffects[effectID]->SetParameterAsFloat(paramID, tmp);
                } else {
                    availableEffects[effectID]->SetParameterRaw(paramID, value);
                }

                ++globalEffectsSettingMemIdx;
            }
        }
        // If we have added a new parameter since the last time settings were stored, we read values from persitent memory (but assume
        // that a new param was added to the end) This way we preserve all other effects and don't get into a scenario where we
        // accidentally use a large value as the number of presets.
        else if (prevParamCount < paramCount) {
            for (uint32_t paramID = 0; paramID < prevParamCount; paramID++) {
                uint32_t value = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
                if (availableEffects[effectID]->GetParameterType(paramID) == ParameterValueType::Float) {
                    float tmp;
                    std::memcpy(&tmp, &value, sizeof(float));
                    availableEffects[effectID]->SetParameterAsFloat(paramID, tmp);
                } else {
                    availableEffects[effectID]->SetParameterRaw(paramID, value);
                }
                ++globalEffectsSettingMemIdx;
            }
            // Shift the array and then add the new values
            globalEffectsMaxIdx = ShiftSettingsToMatchCurrentParameters(prevParamCount, paramCount, effectID, presetsCount,
                                                                        globalEffectsSettingMemIdx, globalEffectsMaxIdx);
            if (globalEffectsMaxIdx == ERR_VALUE_MAX) {
                // The shift couldn't fit inside SETTINGS_ABSOLUTE_MAX_PARAM_COUNT; stop rather than
                // let a poisoned globalEffectsMaxIdx feed (and possibly overflow) a later shift call.
                break;
            }
            for (uint32_t paramID = prevParamCount; paramID < paramCount; paramID++) {
                uint32_t value = availableEffects[effectID]->GetParameterRaw(paramID);
                settings.globalEffectsSettings[globalEffectsSettingMemIdx] = value;
                ++globalEffectsSettingMemIdx;
            }
        }
        // Else, if we have removed a parameter since the last time settings were stored, read all the relevant parameters, (again
        // assume that these were correct in the first place) Then shift the effects after this one back by the appropriate amount of
        // words
        else {
            for (uint32_t paramID = 0; paramID < paramCount; paramID++) {
                uint32_t value = settings.globalEffectsSettings[globalEffectsSettingMemIdx];
                availableEffects[effectID]->SetParameterRaw(paramID, value);
                ++globalEffectsSettingMemIdx;
            }
            globalEffectsMaxIdx = ShiftSettingsToMatchCurrentParameters(prevParamCount, paramCount, effectID, presetsCount,
                                                                        globalEffectsSettingMemIdx, globalEffectsMaxIdx);
            if (globalEffectsMaxIdx == ERR_VALUE_MAX) {
                break;
            }
        }
        availableEffects[effectID]->SetPresetCount(presetsCount);
        /* Assume preset 0 for now */
        availableEffects[effectID]->SetCurrentPreset(0U);

        // Bound the skip to the next effect's header before applying it.
        uint64_t skip = static_cast<uint64_t>(paramCount) * static_cast<uint64_t>(presetsCount - 1U);
        if (static_cast<uint64_t>(globalEffectsSettingMemIdx) + skip > SETTINGS_ABSOLUTE_MAX_PARAM_COUNT) {
            break;
        }
        globalEffectsSettingMemIdx += static_cast<uint32_t>(skip);
    }
}

void SaveEffectSettingsToPersitantStorageForEffectID(int effectID, uint32_t presetID) {
    bool canWriteNewPreset = true;
    Settings &settings = storage.GetSettings();
    uint32_t globalEffectsMaxIdx = settings.globalEffectsSettings[0U];

    // Save Effect Parameters to Persistant Storage based on values from the specified active effect
    if (effectID >= 0 && effectID < availableEffectsCount) {
        int paramCount = availableEffects[effectID]->GetParameterCount();
        uint32_t presetCount = availableEffects[effectID]->GetPresetCount();
        uint32_t startIdx;

        startIdx = availableEffects[effectID]->GetSettingsArrayStartIdx() + 2 + (paramCount * presetID);

        if (presetID >= presetCount) {
            if (presetCount >= kMaxPresetsPerEffect) {
                // Refuse to grow past the same cap SettingsLayoutLooksValid() enforces on load
                // (guitar_pedal_storage.cpp); without this, a save could write a presetsCount
                // word the loader would then reject wholesale on the next boot.
                canWriteNewPreset = false;
            } else {
                // Ignore whatever presetID was given and just increment by 1
                startIdx = availableEffects[effectID]->GetSettingsArrayStartIdx() + 2 + (paramCount * presetCount);
                globalEffectsMaxIdx = ShiftSettingsToAddNewPreset(effectID, paramCount, startIdx, globalEffectsMaxIdx);
                if (globalEffectsMaxIdx == ERR_VALUE_MAX) {
                    // TODO: Log some error message, for now don't do anything and prevent the adding the new preset
                    canWriteNewPreset = false;
                } else {
                    presetCount += 1;
                    availableEffects[effectID]->SetPresetCount(presetCount);
                }
            }
        }

        if (canWriteNewPreset) {
            for (int paramID = 0; paramID < paramCount; paramID++) {
                if (availableEffects[effectID]->GetParameterType(paramID) == ParameterValueType::Float) {
                    uint32_t tmp;
                    float f = availableEffects[effectID]->GetParameterAsFloat(paramID);
                    std::memcpy(&tmp, &f, sizeof(float));
                    SetSettingsParameterValueForEffect(effectID, paramID, tmp, startIdx);
                } else {
                    SetSettingsParameterValueForEffect(effectID, paramID, availableEffects[effectID]->GetParameterRaw(paramID),
                                                       startIdx);
                }
            }
            // Set the new maximum
            settings.globalEffectsSettings[0U] = globalEffectsMaxIdx;
            // Set the index to number of Presets for this effect
            startIdx = availableEffects[effectID]->GetSettingsArrayStartIdx();
            settings.globalEffectsSettings[startIdx] = presetCount;
        }
    }
}

// Helpful Function for setting a parameter value for an effect from the Persistant Storage
void SetSettingsParameterValueForEffect(int effectID, int paramID, uint32_t paramValue, uint32_t startIdx) {
    // Make sure the effect and param id are within valid ranges for the settings.
    if (effectID < 0 || effectID > availableEffectsCount - 1 || paramID < 0) {
        return;
    }
    if (paramID > availableEffects[effectID]->GetParameterCount()) {
        return;
    }

    // Get a handle to the persitance storage settings
    Settings &settings = storage.GetSettings();
    settings.globalEffectsSettings[startIdx + paramID] = paramValue;
}

void FactoryReset(void *context) {
    // Restoring defaults erases and rewrites QSPI flash; start with a full watchdog window.
    // (A no-op if the watchdog was never started.)
    bkshepherd::WatchdogKick();
    storage.RestoreDefaults();
}
