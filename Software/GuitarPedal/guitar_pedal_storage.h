#pragma once
#ifndef GUITAR_PEDAL_STORAGE_H
#define GUITAR_PEDAL_STORAGE_H

// Persistent Storage Settings
// Bumped to 9: added globalEffectListFingerprint (struct layout changed, and any store whose
// fingerprint doesn't match the firmware's current effect list is now factory-reset too --
// see the 2026-09-28 boot-loop note above LoadEffectSettingsFromPersistantStorage() in the .cpp).
#define SETTINGS_FILE_FORMAT_VERSION 9

// Arbitrarily limiting this to 4KB of stored presets since this sits in DTCMRAM which is limited to 128KB.
// TODO: In the future it would be better if this worked with the QSPI directly instead of using
// the PersistentStorage class as an abstraction since that only lets you store a fixed struct size.
// then it would be possible to not have to pre-allocate a fixed size in DTCMRAM for the preset save data.
#define SETTINGS_ABSOLUTE_MAX_PARAM_COUNT 1024
#define ERR_VALUE_MAX 0xffffffff

// Ceiling on how many presets a single effect may have. Enforced both when a new preset would
// be created (SaveEffectSettingsToPersitantStorageForEffectID(), guitar_pedal_storage.cpp) and
// when validating a stored settings table before trusting it (SettingsLayoutLooksValid(),
// guitar_pedal_storage.cpp) -- kept as one constant so the two can't drift apart.
constexpr uint32_t kMaxPresetsPerEffect = 64;

// Save System Variables
struct Settings {
    int fileFormatVersion;
    int globalActiveEffectID;
    bool globalMidiEnabled;
    bool globalMidiThrough;
    int globalMidiChannel;
    bool globalRelayBypassEnabled;
    bool globalSplitMonoInputToStereo;

    // Fingerprint of the compiled-in effect list (names + parameter counts, see
    // ComputeEffectListFingerprint() in guitar_pedal_storage.cpp). A stored settings block whose
    // fingerprint doesn't match the running firmware's effect list was written for a different
    // effect list (e.g. an effect was added/removed/reordered) and is factory-reset rather than
    // trusted, since the per-effect preset/parameter counts it holds would no longer line up with
    // `availableEffects`.
    uint32_t globalEffectListFingerprint;

    // Set aside a block of memory for individual effect params.
    // Please note this MUST be a fixed amount of memory in the struct and cannot be a pointer to dynamic memory!
    // If you try to use a pointer it will only save the pointer address to QSPI storage and not any of the contents
    // of that dynamic memory.  This is a limitation of the way the PersistantStorage helper class works.
    uint32_t globalEffectsSettings[SETTINGS_ABSOLUTE_MAX_PARAM_COUNT];

    bool operator==(const Settings &rhs) {
        if (fileFormatVersion != rhs.fileFormatVersion || globalActiveEffectID != rhs.globalActiveEffectID ||
            globalMidiEnabled != rhs.globalMidiEnabled || globalMidiThrough != rhs.globalMidiThrough ||
            globalMidiChannel != rhs.globalMidiChannel || globalRelayBypassEnabled != rhs.globalRelayBypassEnabled ||
            globalSplitMonoInputToStereo != rhs.globalSplitMonoInputToStereo ||
            globalEffectListFingerprint != rhs.globalEffectListFingerprint) {
            return false;
        }

        for (uint32_t i = 0; i < SETTINGS_ABSOLUTE_MAX_PARAM_COUNT; i++) {
            if (globalEffectsSettings[i] != rhs.globalEffectsSettings[i]) {
                return false;
            }
        }

        return true;
    }

    bool operator!=(const Settings &rhs) { return !operator==(rhs); }
};

void InitPersistantStorage();
void LoadEffectSettingsFromPersistantStorage();
void SaveEffectSettingsToPersitantStorageForEffectID(int effectID, uint32_t presetID);
void SetSettingsParameterValueForEffect(int effectID, int paramID, uint32_t paramValue, uint32_t startIdx);
void LoadPresetFromPersistentStorage(uint32_t effectID, uint32_t presetID);
void FactoryReset(void *context);

#endif
