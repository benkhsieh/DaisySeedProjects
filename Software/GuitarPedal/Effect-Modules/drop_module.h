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
