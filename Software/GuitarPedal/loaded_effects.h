// Edit the contents of this file to populate the available effects that you
// want to use
//
// ============================================================
// HOW TO ADD A NEW EFFECT MODULE
// ============================================================
// Adding an effect requires touching FOUR places in order:
//
//   STEP 1: Create the effect files
//             Effect-Modules/my_effect_module.h
//             Effect-Modules/my_effect_module.cpp
//
//   STEP 2: Add the #include below in the "Include all effect modules" section
//             #include "Effect-Modules/my_effect_module.h"
//
//   STEP 3: Add instantiation in the effectList array in load_effects() below
//             new MyEffectModule(),
//
//   STEP 4: Add the .cpp source in Makefile (see CPP_SOURCES section there)
//             CPP_SOURCES += Effect-Modules/my_effect_module.cpp
//
// NOTE: Steps 2-4 must ALL be done. Missing any one step causes silent
//       failures (missing include = compile error; missing array entry =
//       effect never available; missing Makefile entry = linker error).
// ============================================================

#ifndef LOADED_EFFECTS_H
#define LOADED_EFFECTS_H
#pragma once

#include "Effect-Modules/base_effect_module.h"

// Include all effect modules
#include "Effect-Modules/amp_module.h"
#include "Effect-Modules/autopan_module.h"
#include "Effect-Modules/chopper_module.h"
#include "Effect-Modules/chorus_module.h"
#include "Effect-Modules/cloudseed_module.h" // Takes up significant SDRAM (about 30%)
#include "Effect-Modules/compressor_module.h"
#include "Effect-Modules/delay_module.h"
#include "Effect-Modules/distortion_module.h"
#include "Effect-Modules/drum_module.h"
#include "Effect-Modules/flanger_module.h"
#include "Effect-Modules/geq_module.h"
#include "Effect-Modules/granulardelay_module.h"
#include "Effect-Modules/ir_module.h"
#include "Effect-Modules/looper_module.h"
#include "Effect-Modules/metro_module.h"
#include "Effect-Modules/modulated_tremolo_module.h"
#include "Effect-Modules/multi_delay_module.h"
#include "Effect-Modules/nam_module.h"
#include "Effect-Modules/noise_gate_module.h"
#include "Effect-Modules/overdrive_module.h"
#include "Effect-Modules/peq_module.h"
#include "Effect-Modules/phaser_module.h"
#include "Effect-Modules/pitch_shifter_module.h"
#include "Effect-Modules/polyoctave_module.h"
#include "Effect-Modules/reverb_module.h"
#include "Effect-Modules/scifi_module.h"
#include "Effect-Modules/spectral_delay_module.h"
#include "Effect-Modules/tuner_module.h"

// Keyboard modules
// #include "Effect-Modules/fm_keys_module.h"
// #include "Effect-Modules/midi_keys_module.h"
// #include "Effect-Modules/modal_keys_module.h"
// #include "Effect-Modules/pluckecho_module.h"
// #include "Effect-Modules/string_keys_module.h"

namespace bkshepherd {

void load_effects(int &availableEffectsCount, BaseEffectModule **&availableEffects) {
    // clang-format off
    static BaseEffectModule* effectList[] = {
        new ModulatedTremoloModule(),
        new OverdriveModule(),
        new AutoPanModule(),
        new ChorusModule(),
        new ChopperModule(),
        new ReverbModule(),
        new MultiDelayModule(),
        new MetroModule(),
        new TunerModule(),
        new PitchShifterModule(),
        new CompressorModule(),
        new LooperModule(),
        new GraphicEQModule(),
        new ParametricEQModule(),
        new NoiseGateModule(),
        new CloudSeedModule(),
        new AmpModule(),
        new DelayModule(),
        new NamModule(),
        new SciFiModule(),
        new PolyOctaveModule(),
        new SpectralDelayModule(),
        new DistortionModule(),
        new GranularDelayModule(), 
        new IrModule(),
        new DrumModule(),  // This module can be used with MIDI keyboard as a drum machine
        new PhaserModule(),
        new FlangerModule(),

        // The following require a MIDI keyboard
        // new MidiKeysModule(),
        // new PluckEchoModule(),
        // new StringKeysModule(),
        // new ModalKeysModule(),
        // new FmKeysModule(),
    };
    // clang-format on

    availableEffectsCount = sizeof(effectList) / sizeof(effectList[0]);
    availableEffects = effectList;
}

} // namespace bkshepherd

#endif
