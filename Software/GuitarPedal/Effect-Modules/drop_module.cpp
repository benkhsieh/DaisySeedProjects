#include "drop_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Util/pitch_shifter.h"

using namespace bkshepherd;

static const char *s_semitoneBinNames[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
static const char *s_modeBinNames[2] = {"LATCH", "MOMENT"};

// Same delay-size range as the Pitch module: shorter is lower latency, longer sounds
// better on wide intervals. ~42 ms at 2048 samples, ~125 ms at 6000.
const uint32_t k_minDelaySamples = 2048;
const uint32_t k_maxDelaySamples = 6000;

// Momentary ramp time each way.
const float k_rampSeconds = 0.1f;

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Semitones",
        valueType : ParameterValueType::Binned,
        valueBinCount : 12,
        valueBinNames : s_semitoneBinNames,
        defaultValue : {.uint_value = 1}, // bin index 1 = "2"
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 1,
        midiCCMapping : -1
    },
    {
        name : "Mode",
        valueType : ParameterValueType::Binned,
        valueBinCount : 2,
        valueBinNames : s_modeBinNames,
        defaultValue : {.uint_value = 0},
        knobMapping : 2,
        midiCCMapping : -1
    },
};

// The pitch shifter's two delay buffers. SDRAM so DTCM (the stack) is untouched.
float DSY_SDRAM_BSS drop_buffer_a[k_maxDelaySamples];
float DSY_SDRAM_BSS drop_buffer_b[k_maxDelaySamples];

namespace {
// The shifter and crossfade are heap-allocated with the module (via new in load_effects),
// so they live in a small holder reached through the module's m_engine pointer rather
// than as file-scope statics in DTCM.
struct DropEngine {
    daisysp_modified::PitchShifter shifter;
    CrossFade mix;
};
} // namespace

DropModule::DropModule() : BaseEffectModule() {
    m_name = "Drop";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

DropModule::~DropModule() {}

float DropModule::SemitonesDown() const { return static_cast<float>(GetParameterAsBinnedValue(0)); }

void DropModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_sampleRate = sample_rate;

    if (m_engine == nullptr) {
        m_engine = new DropEngine(); // once, at boot; heap lives in D2 SRAM
    }
    DropEngine *e = static_cast<DropEngine *>(m_engine);

    memset(drop_buffer_a, 0, sizeof(drop_buffer_a));
    memset(drop_buffer_b, 0, sizeof(drop_buffer_b));
    e->shifter.Init(sample_rate, drop_buffer_a, drop_buffer_b, k_maxDelaySamples);
    e->mix.Init(CROSSFADE_CPOW);
    e->mix.SetPos(GetParameterAsFloat(1));

    m_latching = GetParameterAsBinnedValue(2) == 1;
    m_rampStepPerSample = 1.0f / (k_rampSeconds * sample_rate);
    m_rampPosition = m_latching ? 1.0f : 0.0f;
    ApplyInterval();
}

void DropModule::ApplyInterval() {
    m_semitonesDown = SemitonesDown();
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    // Longer delay for wider intervals, as the Pitch module does in latch mode.
    const float t = (m_semitonesDown - 1.0f) / 11.0f;
    const uint32_t delaySize =
        static_cast<uint32_t>(std::lerp(static_cast<float>(k_minDelaySamples), static_cast<float>(k_maxDelaySamples), t));
    e->shifter.SetDelSize(std::clamp(delaySize, k_minDelaySamples, k_maxDelaySamples));
}

void DropModule::ParameterChanged(int parameter_id) {
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (parameter_id == 0) {
        ApplyInterval();
    } else if (parameter_id == 1 && e != nullptr) {
        e->mix.SetPos(GetParameterAsFloat(1));
    } else if (parameter_id == 2) {
        m_latching = GetParameterAsBinnedValue(2) == 1;
        // Switching to LATCH engages fully; switching to MOMENT follows the footswitch.
        m_rampPosition = m_latching ? 1.0f : (m_altHeld ? 1.0f : 0.0f);
    }
}

void DropModule::AlternateFootswitchPressed() { m_altHeld = true; }

void DropModule::AlternateFootswitchReleased() { m_altHeld = false; }

void DropModule::ProcessMono(float in) {
    BaseEffectModule::ProcessMono(in);

    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }

    // Ramp toward the target in MOMENT mode; pinned at 1 in LATCH mode.
    const float target = m_latching ? 1.0f : (m_altHeld ? 1.0f : 0.0f);
    if (m_rampPosition < target) {
        m_rampPosition = std::min(target, m_rampPosition + m_rampStepPerSample);
    } else if (m_rampPosition > target) {
        m_rampPosition = std::max(target, m_rampPosition - m_rampStepPerSample);
    }

    e->shifter.SetTransposition(-m_semitonesDown * m_rampPosition);
    float shifted = e->shifter.Process(in); // non-const: CrossFade::Process takes float&
    const float out = e->mix.Process(in, shifted);
    m_audioLeft = m_audioRight = out;
}

void DropModule::ProcessStereo(float inL, float inR) { ProcessMono(inL); }

float DropModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return (m_rampPosition > 0.5f) ? value : 0.0f;
    }
    return value;
}

void DropModule::Reset() {
    DropEngine *e = static_cast<DropEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    memset(drop_buffer_a, 0, sizeof(drop_buffer_a));
    memset(drop_buffer_b, 0, sizeof(drop_buffer_b));
    e->shifter.Init(m_sampleRate, drop_buffer_a, drop_buffer_b, k_maxDelaySamples);
    ApplyInterval();
    m_rampPosition = m_latching ? 1.0f : 0.0f;
}
