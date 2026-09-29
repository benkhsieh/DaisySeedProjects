#include "amp_tremolo_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

// SoftClip(1.5) (DaisySP's SoftLimit polynomial at x = 1.5). Dividing by it rescales the
// soft-clipped sine back to exactly -1..1, so full Intensity reaches silence at the trough
// and the crest is unity gain. constexpr, so it takes no storage.
constexpr float k_shapePeak = 1.5f * (27.0f + 1.5f * 1.5f) / (27.0f + 9.0f * 1.5f * 1.5f);

static const int s_paramCount = 2;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Speed",
        valueType : ParameterValueType::Float,
        valueCurve : ParameterValueCurve::Log,
        valueBinCount : 0,
        defaultValue : {.float_value = 5.0f},
        knobMapping : 0,
        midiCCMapping : 20,
        minValue : 1,
        maxValue : 12
    },
    {
        name : "Intensity",
        valueType : ParameterValueType::Float,
        valueCurve : ParameterValueCurve::Linear,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 1,
        midiCCMapping : 21,
        minValue : 0,
        maxValue : 1
    },
};

AmpTremoloModule::AmpTremoloModule() : BaseEffectModule() {
    m_name = "AmpTrem";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

AmpTremoloModule::~AmpTremoloModule() {}

void AmpTremoloModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_lfo.Init(sample_rate);
    m_lfo.SetWaveform(Oscillator::WAVE_SIN);
    m_lfo.SetAmp(1.0f);
    ApplySpeed();
    m_gain = 1.0f;
}

void AmpTremoloModule::ApplySpeed() {
    float hz = GetParameterAsFloat(0);
    if (hz < m_speedMinHz) {
        hz = m_speedMinHz;
    } else if (hz > m_speedMaxHz) {
        hz = m_speedMaxHz;
    }
    m_lfo.SetFreq(hz);
}

void AmpTremoloModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 0) {
        ApplySpeed();
    }
}

void AmpTremoloModule::ProcessMono(float in) {
    BaseEffectModule::ProcessMono(in);

    // Sine LFO with the peaks softly flattened: an optical tremolo spends a little longer
    // near full volume than a pure sine does.
    const float lfo = SoftClip(1.5f * m_lfo.Process()) / k_shapePeak; // -1..1, flattened top and bottom
    const float depth = GetParameterAsFloat(1);
    const float target = 1.0f - depth * 0.5f * (1.0f - lfo); // 1 - depth .. 1

    // Ease toward the target so square-ish edges never click.
    fonepole(m_gain, target, 0.01f);

    m_audioLeft = m_audioLeft * m_gain;
    m_audioRight = m_audioLeft;
}

void AmpTremoloModule::ProcessStereo(float inL, float inR) {
    ProcessMono(inL);
    BaseEffectModule::ProcessStereo(m_audioLeft, inR);
    m_audioRight = m_audioRight * m_gain;
}

void AmpTremoloModule::SetTempo(uint32_t bpm) {
    // One LFO cycle per beat.
    float hz = tempo_to_freq(bpm);
    if (hz < m_speedMinHz) {
        hz = m_speedMinHz;
    } else if (hz > m_speedMaxHz) {
        hz = m_speedMaxHz;
    }
    SetParameterAsFloat(0, hz);
    ApplySpeed();
}

float AmpTremoloModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return value * m_gain;
    }
    return value;
}

void AmpTremoloModule::Reset() {
    m_lfo.Reset();
    m_gain = 1.0f;
}
