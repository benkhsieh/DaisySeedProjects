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
