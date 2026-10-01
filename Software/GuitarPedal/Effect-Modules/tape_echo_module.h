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
    void SetEnabled(bool isEnabled) override;
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
    float m_tapeMod = 0.0f;               // smoothed wow/flutter value from TapeModulator (additive, near 0)
    bool m_oscillateHeld = false;         // Alt held: repeats forced to maximum
    float m_ledPhase = 0.0f;
};
} // namespace bkshepherd
#endif
#endif
