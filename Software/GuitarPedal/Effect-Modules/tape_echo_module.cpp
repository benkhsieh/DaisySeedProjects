#include "tape_echo_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Util/audio_guard.h"
#include "../Util/audio_utilities.h"
#include "../Util/tape_modulator.h"
#include "../Util/tape_saturator.h"

using namespace bkshepherd;

static const char *s_headBinNames[7] = {"1", "2", "3", "1+2", "2+3", "1+2+3", "1+3"};

// Head 3 sits at 3x the head-1 time; 800 ms x 3 at 48 kHz = 115200 samples, plus wow room.
const size_t k_maxDelaySamples = 120000;
const float k_minRateMs = 40.0f;
const float k_maxRateMs = 800.0f;
const float k_maxFeedback = 1.1f;
const float k_toneMinHz = 800.0f;
const float k_toneMaxHz = 12000.0f;

// TapeModulator::GetTapeSpeed returns an additive deviation around 0 (not a factor around
// 1). The Delay module's "Tape" wave adds it to the delay time scaled by 500 samples per
// unit (DelayModule::ProcessModulation); the same scale is used here.
const float k_tapeModDepthSamples = 500.0f;

static const int s_paramCount = 6;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Rate",
        valueType : ParameterValueType::Float,
        valueCurve : ParameterValueCurve::Log,
        valueBinCount : 0,
        defaultValue : {.float_value = 120.0f},
        knobMapping : 0,
        midiCCMapping : 14,
        minValue : 40,
        maxValue : 800
    },
    {
        name : "Repeats",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.25f},
        knobMapping : 1,
        midiCCMapping : 15
    },
    {
        name : "Echo Vol",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.55f},
        knobMapping : 2,
        midiCCMapping : 16
    },
    {
        name : "Wow Flut",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.15f},
        knobMapping : 3,
        midiCCMapping : 17
    },
    {
        name : "Tone",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.6f},
        knobMapping : 4,
        midiCCMapping : 18
    },
    {
        name : "Heads",
        valueType : ParameterValueType::Binned,
        valueBinCount : 7,
        valueBinNames : s_headBinNames,
        defaultValue : {.uint_value = 0},
        knobMapping : 5,
        midiCCMapping : 19
    },
};

DelayLine<float, k_maxDelaySamples> DSY_SDRAM_BSS tape_echo_line;

namespace {
struct TapeEchoEngine {
    TapeModulator wowFlutter;
    Tone toneFilter;
    Oscillator led;
};
} // namespace

TapeEchoModule::TapeEchoModule() : BaseEffectModule() {
    m_name = "TapeEcho";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

TapeEchoModule::~TapeEchoModule() {}

float TapeEchoModule::RateSamples() const {
    float ms = GetParameterAsFloat(0);
    ms = std::clamp(ms, k_minRateMs, k_maxRateMs);
    return ms * 0.001f * m_sampleRate;
}

float TapeEchoModule::FeedbackAmount() const {
    if (m_oscillateHeld) {
        return k_maxFeedback;
    }
    return GetParameterAsFloat(1) * k_maxFeedback;
}

void TapeEchoModule::HeadGains(float &g1, float &g2, float &g3) const {
    switch (GetParameterAsBinnedValue(5)) {
    case 1:
        g1 = 1;
        g2 = 0;
        g3 = 0;
        break;
    case 2:
        g1 = 0;
        g2 = 1;
        g3 = 0;
        break;
    case 3:
        g1 = 0;
        g2 = 0;
        g3 = 1;
        break;
    case 4:
        g1 = 1;
        g2 = 1;
        g3 = 0;
        break;
    case 5:
        g1 = 0;
        g2 = 1;
        g3 = 1;
        break;
    case 6:
        g1 = 1;
        g2 = 1;
        g3 = 1;
        break;
    case 7:
        g1 = 1;
        g2 = 0;
        g3 = 1;
        break;
    default:
        g1 = 1;
        g2 = 0;
        g3 = 0;
        break;
    }
    // Keep the summed level roughly constant as heads are added.
    const float n = g1 + g2 + g3;
    if (n > 1.0f) {
        const float s = 1.0f / n;
        g1 *= s;
        g2 *= s;
        g3 *= s;
    }
}

void TapeEchoModule::ApplyTone() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    const float t = std::clamp(GetParameterAsFloat(4), 0.0f, 1.0f);
    const float hz = k_toneMinHz * std::pow(k_toneMaxHz / k_toneMinHz, t); // log sweep
    e->toneFilter.SetFreq(hz);
}

void TapeEchoModule::ApplyLedRate() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    e->led.SetFreq(m_sampleRate / RateSamples());
}

void TapeEchoModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_sampleRate = sample_rate;
    if (m_engine == nullptr) {
        m_engine = new TapeEchoEngine(); // once, at boot; heap lives in D2 SRAM
    }
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    tape_echo_line.Init();
    e->wowFlutter.Init(sample_rate);
    e->toneFilter.Init(sample_rate);
    e->led.Init(sample_rate);
    e->led.SetWaveform(Oscillator::WAVE_SQUARE);
    e->led.SetAmp(1.0f);
    m_currentRateSamples = RateSamples();
    m_tapeMod = 0.0f;
    m_oscillateHeld = false;
    ApplyTone();
    ApplyLedRate();
}

void TapeEchoModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 0) {
        ApplyLedRate();
    } else if (parameter_id == 4) {
        ApplyTone();
    }
}

void TapeEchoModule::ProcessMono(float in) {
    BaseEffectModule::ProcessMono(in);
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }

    // Delay time glides toward the knob so changes sweep like a tape motor, not click.
    fonepole(m_currentRateSamples, RateSamples(), 0.0005f);

    // Wow and flutter: same rates the Delay module uses for its "Tape" wave, with the
    // depth scaled by the Wow Flut knob (0 = clean digital). See DelayModule::ProcessModulation.
    const float wf = GetParameterAsFloat(3);
    const float depth = 2.0f * wf;
    const float speed = e->wowFlutter.GetTapeSpeed(0.6f, 3.0f, depth, depth);
    fonepole(m_tapeMod, speed, 0.01f);

    const float head1 =
        std::clamp(m_currentRateSamples + m_tapeMod * k_tapeModDepthSamples, 1.0f, static_cast<float>(k_maxDelaySamples / 3 - 2));
    float g1, g2, g3;
    HeadGains(g1, g2, g3);
    const float tap1 = g1 > 0.0f ? tape_echo_line.Read(head1) : 0.0f;
    const float tap2 = g2 > 0.0f ? tape_echo_line.Read(head1 * 2.0f) : 0.0f;
    const float tap3 = g3 > 0.0f ? tape_echo_line.Read(head1 * 3.0f) : 0.0f;
    const float heads = g1 * tap1 + g2 * tap2 + g3 * tap3;

    // Feedback path: saturate (so runaway repeats compress), then darken.
    float fb = TapeSaturate(heads * FeedbackAmount(), 1.5f);
    fb = e->toneFilter.Process(fb);
    if (!audio_guard::IsFiniteBits(fb)) {
        fb = 0.0f;
    }
    tape_echo_line.Write(in + fb);

    const float wet = heads * GetParameterAsFloat(2);
    m_audioLeft = in + wet;
    m_audioRight = m_audioLeft;

    // LED pulses at the delay rate.
    m_ledPhase = e->led.Process();
}

void TapeEchoModule::ProcessStereo(float inL, float inR) {
    ProcessMono(inL);
    BaseEffectModule::ProcessStereo(m_audioLeft, inR);
    m_audioRight = m_audioLeft; // same echo on both channels (spec 6.3)
}

void TapeEchoModule::SetTempo(uint32_t bpm) {
    // One repeat per beat, clamped to the knob range.
    const float hz = tempo_to_freq(bpm);
    float ms = 1000.0f / std::max(hz, 0.001f);
    ms = std::clamp(ms, k_minRateMs, k_maxRateMs);
    SetParameterAsFloat(0, ms);
    ApplyLedRate();
}

void TapeEchoModule::AlternateFootswitchHeldFor1Second() { m_oscillateHeld = true; }

void TapeEchoModule::AlternateFootswitchReleased() { m_oscillateHeld = false; }

void TapeEchoModule::SetEnabled(bool isEnabled) {
    // Alt events only reach the active, engaged effect, so a release while bypassed or
    // after switching away would never arrive. Drop the held state whenever disengaged so
    // the echo does not come back self-oscillating on the next engage.
    if (!isEnabled) {
        m_oscillateHeld = false;
    }
    BaseEffectModule::SetEnabled(isEnabled);
}

float TapeEchoModule::GetBrightnessForLED(int led_id) const {
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1) {
        return (m_ledPhase > 0.0f) ? value : 0.0f;
    }
    return value;
}

void TapeEchoModule::Reset() {
    TapeEchoEngine *e = static_cast<TapeEchoEngine *>(m_engine);
    if (e == nullptr) {
        return;
    }
    tape_echo_line.Reset();
    e->toneFilter.Init(m_sampleRate);
    ApplyTone();
    m_currentRateSamples = RateSamples();
    m_tapeMod = 0.0f;
    m_oscillateHeld = false;
}
