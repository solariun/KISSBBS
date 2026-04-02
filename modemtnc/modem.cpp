// Modem demodulator & modulator — AFSK 1200/300, GMSK 9600
// DSP algorithms derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Original: https://github.com/wb2osz/direwolf
// Simplified for modemtnc: single-decoder, C++ classes, no global state
#include "modem.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// PLL: 32-bit phase accumulator wraps at 2^32
static constexpr double TICKS_PER_PLL_CYCLE = 256.0 * 256.0 * 256.0 * 256.0;

namespace modem {

// ===========================================================================
//  DSP primitives
// ===========================================================================

void Demodulator::push_sample(float val, float* buf, int size) {
    memmove(buf + 1, buf, (size - 1) * sizeof(float));
    buf[0] = val;
}

float Demodulator::convolve(const float* data, const float* filter, int taps) {
    float sum = 0.0f;
    for (int j = 0; j < taps; j++)
        sum += filter[j] * data[j];
    return sum;
}

float Demodulator::agc(float in, float fast_attack, float slow_decay, float* peak, float* valley) {
    if (in >= *peak)
        *peak = in * fast_attack + *peak * (1.0f - fast_attack);
    else
        *peak = in * slow_decay + *peak * (1.0f - slow_decay);

    if (in <= *valley)
        *valley = in * fast_attack + *valley * (1.0f - fast_attack);
    else
        *valley = in * slow_decay + *valley * (1.0f - slow_decay);

    float x = in;
    if (x > *peak) x = *peak;
    if (x < *valley) x = *valley;

    if (*peak > *valley)
        return (x - 0.5f * (*peak + *valley)) / (*peak - *valley);
    return 0.0f;
}

int Demodulator::descramble(int in, int* state) {
    int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
    *state = (*state << 1) | (in & 1);
    return out;
}

// ===========================================================================
//  Filter generation (from Direwolf dsp.c)
// ===========================================================================

static float window_func(int j, int size) {
    // Hamming window
    return 0.54f - 0.46f * cosf(2.0f * (float)M_PI * j / (size - 1));
}

void Demodulator::gen_lowpass(float fc, float* filter, int taps) {
    float center = 0.5f * (taps - 1);
    for (int j = 0; j < taps; j++) {
        float t = j - center;
        if (fabsf(t) < 0.001f)
            filter[j] = 2.0f * fc;
        else
            filter[j] = sinf(2.0f * (float)M_PI * fc * t) / ((float)M_PI * t);
        filter[j] *= window_func(j, taps);
    }
    // Normalize
    float sum = 0;
    for (int j = 0; j < taps; j++) sum += filter[j];
    if (fabsf(sum) > 0.001f)
        for (int j = 0; j < taps; j++) filter[j] /= sum;
}

void Demodulator::gen_bandpass(float f1, float f2, float* filter, int taps) {
    float center = 0.5f * (taps - 1);
    for (int j = 0; j < taps; j++) {
        float t = j - center;
        float h;
        if (fabsf(t) < 0.001f)
            h = 2.0f * (f2 - f1);
        else
            h = (sinf(2.0f * (float)M_PI * f2 * t) - sinf(2.0f * (float)M_PI * f1 * t)) / ((float)M_PI * t);
        filter[j] = h * window_func(j, taps);
    }
    // Normalize for unity gain at center frequency
    float w = 2.0f * (float)M_PI * (f1 + f2) / 2.0f;
    float sum = 0;
    for (int j = 0; j < taps; j++)
        sum += filter[j] * cosf(w * (j - 0.5f * (taps - 1)));
    if (fabsf(sum) > 0.001f)
        for (int j = 0; j < taps; j++) filter[j] /= sum;
}

void Demodulator::gen_rrc_lowpass(float* filter, int taps, float rolloff, float sps) {
    float center = 0.5f * (taps - 1);
    for (int j = 0; j < taps; j++) {
        float t = (j - center) / sps;  // in symbol periods
        float h;
        if (fabsf(t) < 0.0001f) {
            h = 1.0f - rolloff + 4.0f * rolloff / (float)M_PI;
        } else if (fabsf(fabsf(t) - 1.0f / (4.0f * rolloff)) < 0.001f && rolloff > 0.001f) {
            h = (rolloff / sqrtf(2.0f)) *
                ((1.0f + 2.0f / (float)M_PI) * sinf((float)M_PI / (4.0f * rolloff)) +
                 (1.0f - 2.0f / (float)M_PI) * cosf((float)M_PI / (4.0f * rolloff)));
        } else {
            float num = sinf((float)M_PI * t * (1.0f - rolloff)) +
                        4.0f * rolloff * t * cosf((float)M_PI * t * (1.0f + rolloff));
            float den = (float)M_PI * t * (1.0f - (4.0f * rolloff * t) * (4.0f * rolloff * t));
            h = (fabsf(den) > 0.0001f) ? num / den : 1.0f;
        }
        filter[j] = h * window_func(j, taps);
    }
    // Normalize
    float sum = 0;
    for (int j = 0; j < taps; j++) sum += filter[j];
    if (fabsf(sum) > 0.001f)
        for (int j = 0; j < taps; j++) filter[j] /= sum;
}

// ===========================================================================
//  Demodulator init
// ===========================================================================

void Demodulator::init(Type type, int sample_rate) {
    type_ = type;
    sample_rate_ = sample_rate;
    dcd_ = false;
    dcd_shreg_ = 0;
    dcd_count_ = 0;
    dcd_missing_ = 0;
    data_clock_pll_ = 0;
    prev_d_c_pll_ = 0;
    prev_demod_data_ = 0;
    prev_demod_bit_ = 0;

    // Init cosine table
    for (int j = 0; j < 256; j++)
        cos256_[j] = cosf((float)j * 2.0f * (float)M_PI / 256.0f);

    switch (type) {
        case AFSK_1200: init_afsk(1200, 1200, 2200); break;
        case AFSK_300:  init_afsk(300, 1600, 1800);  break;
        case EAS:       init_afsk(521, 2083, 1563);   break;
        case GMSK_9600:
        case AIS:       init_9600(); break;
        default:        init_afsk(1200, 1200, 2200); break;
    }
}

void Demodulator::set_type(Type type) {
    init(type, sample_rate_);
}

void Demodulator::init_afsk(int baud, int mark, int space) {
    memset(&afsk_, 0, sizeof(afsk_));

    // PLL step
    pll_step_per_sample_ = (int)round(TICKS_PER_PLL_CYCLE * (double)baud / (double)sample_rate_);
    pll_locked_inertia_ = 0.74f;
    pll_searching_inertia_ = 0.50f;

    // Use Profile B (FM discriminator) — more robust for FM signals with de-emphasis
    afsk_.use_profile_b = true;

    // Profile A: Mark/Space local oscillators (kept for reference)
    afsk_.m_osc_phase = 0;
    afsk_.m_osc_delta = (unsigned int)round(pow(2.0, 32.0) * (double)mark / (double)sample_rate_);
    afsk_.s_osc_phase = 0;
    afsk_.s_osc_delta = (unsigned int)round(pow(2.0, 32.0) * (double)space / (double)sample_rate_);

    // Profile B: Center frequency oscillator
    afsk_.c_osc_phase = 0;
    afsk_.c_osc_delta = (unsigned int)round(pow(2.0, 32.0) * 0.5 * (mark + space) / (double)sample_rate_);
    afsk_.prev_phase = 0;
    afsk_.normalize_rpsam = (float)(1.0 / (0.5 * abs(mark - space) * 2 * M_PI / sample_rate_));
    memset(afsk_.c_I_raw, 0, sizeof(afsk_.c_I_raw));
    memset(afsk_.c_Q_raw, 0, sizeof(afsk_.c_Q_raw));

    // Pre-filter (bandpass around mark/space)
    afsk_.use_prefilter = true;
    float prebaud = (baud > 600) ? 0.155f : 0.87f;
    float prefilt_sym = (baud > 600) ? (383.0f * 1200.0f / 44100.0f) : 1.857f;

    afsk_.pre_filter_taps = ((int)(prefilt_sym * (float)sample_rate_ / (float)baud)) | 1;
    if (afsk_.pre_filter_taps > MAX_FILTER) afsk_.pre_filter_taps = (MAX_FILTER - 1) | 1;

    float f1 = ((float)std::min(mark, space) - prebaud * baud) / (float)sample_rate_;
    float f2 = ((float)std::max(mark, space) + prebaud * baud) / (float)sample_rate_;
    if (f1 < 0.001f) f1 = 0.001f;
    gen_bandpass(f1, f2, afsk_.pre_filter, afsk_.pre_filter_taps);

    // RRC low-pass filter (Profile B uses wider filter)
    float rrc_width = afsk_.use_profile_b ? 2.00f : 2.80f;
    float rrc_rolloff = afsk_.use_profile_b ? 0.40f : 0.20f;
    afsk_.lp_filter_taps = ((int)(rrc_width * (float)sample_rate_ / baud)) | 1;
    if (afsk_.lp_filter_taps > MAX_FILTER) afsk_.lp_filter_taps = (MAX_FILTER - 1) | 1;
    gen_rrc_lowpass(afsk_.lp_filter, afsk_.lp_filter_taps, rrc_rolloff, (float)sample_rate_ / baud);

    // AGC
    afsk_.agc_fast_attack = 0.70f;
    afsk_.agc_slow_decay = 0.000090f;
    afsk_.m_peak = afsk_.m_valley = 0;
    afsk_.s_peak = afsk_.s_valley = 0;
}

void Demodulator::init_9600() {
    memset(&bb_, 0, sizeof(bb_));

    pll_step_per_sample_ = (int)round(TICKS_PER_PLL_CYCLE * 9600.0 / (double)sample_rate_);
    pll_locked_inertia_ = 0.74f;
    pll_searching_inertia_ = 0.50f;

    // Low-pass filter — wider cutoff for baseband (need to pass the data bandwidth)
    float fc = 9600.0f * 0.7f / (float)sample_rate_;
    bb_.lp_filter_taps = ((int)(0.8f * (float)sample_rate_ / 9600.0f)) | 1;
    if (bb_.lp_filter_taps < 3) bb_.lp_filter_taps = 3;
    if (bb_.lp_filter_taps > MAX_FILTER) bb_.lp_filter_taps = (MAX_FILTER - 1) | 1;
    gen_lowpass(fc, bb_.lp_filter, bb_.lp_filter_taps);

    bb_.scramble_state = 0;
    bb_.agc_fast_attack = 0.70f;
    bb_.agc_slow_decay = 0.000090f;
    bb_.peak = bb_.valley = 0;
}

// ===========================================================================
//  Demodulator process sample
// ===========================================================================

void Demodulator::process_sample(int16_t sample) {
    float fsam = (float)sample / 16384.0f;

    switch (type_) {
        case AFSK_1200:
        case AFSK_300:
        case EAS:
            process_afsk(fsam);
            break;
        case GMSK_9600:
        case AIS:
            process_9600(fsam);
            break;
        default:
            process_afsk(fsam);
            break;
    }
}

void Demodulator::process_afsk(float fsam) {
    // Optional bandpass pre-filter
    if (afsk_.use_prefilter) {
        push_sample(fsam, afsk_.raw_cb, afsk_.pre_filter_taps);
        fsam = convolve(afsk_.raw_cb, afsk_.pre_filter, afsk_.pre_filter_taps);
    }

    float demod_out;

    if (afsk_.use_profile_b) {
        // ── Profile B: FM discriminator ──
        // Mix with center frequency, measure phase rate.
        // More robust than mark/space amplitude comparison for FM signals.
        push_sample(fsam * fcos256(afsk_.c_osc_phase), afsk_.c_I_raw, afsk_.lp_filter_taps);
        push_sample(fsam * fsin256(afsk_.c_osc_phase), afsk_.c_Q_raw, afsk_.lp_filter_taps);
        afsk_.c_osc_phase += afsk_.c_osc_delta;

        float c_I = convolve(afsk_.c_I_raw, afsk_.lp_filter, afsk_.lp_filter_taps);
        float c_Q = convolve(afsk_.c_Q_raw, afsk_.lp_filter, afsk_.lp_filter_taps);

        float phase = atan2f(c_Q, c_I);
        float rate = phase - afsk_.prev_phase;
        if (rate > (float)M_PI) rate -= 2.0f * (float)M_PI;
        else if (rate < -(float)M_PI) rate += 2.0f * (float)M_PI;
        afsk_.prev_phase = phase;

        // Normalize: mark → +1, space → -1
        demod_out = rate * afsk_.normalize_rpsam;

    } else {
        // ── Profile A: Mark/Space amplitude comparison ──
        push_sample(fsam * fcos256(afsk_.m_osc_phase), afsk_.m_I_raw, afsk_.lp_filter_taps);
        push_sample(fsam * fsin256(afsk_.m_osc_phase), afsk_.m_Q_raw, afsk_.lp_filter_taps);
        afsk_.m_osc_phase += afsk_.m_osc_delta;

        push_sample(fsam * fcos256(afsk_.s_osc_phase), afsk_.s_I_raw, afsk_.lp_filter_taps);
        push_sample(fsam * fsin256(afsk_.s_osc_phase), afsk_.s_Q_raw, afsk_.lp_filter_taps);
        afsk_.s_osc_phase += afsk_.s_osc_delta;

        float m_I = convolve(afsk_.m_I_raw, afsk_.lp_filter, afsk_.lp_filter_taps);
        float m_Q = convolve(afsk_.m_Q_raw, afsk_.lp_filter, afsk_.lp_filter_taps);
        float m_amp = hypotf(m_I, m_Q);

        float s_I = convolve(afsk_.s_I_raw, afsk_.lp_filter, afsk_.lp_filter_taps);
        float s_Q = convolve(afsk_.s_Q_raw, afsk_.lp_filter, afsk_.lp_filter_taps);
        float s_amp = hypotf(s_I, s_Q);

        float m_norm = agc(m_amp, afsk_.agc_fast_attack, afsk_.agc_slow_decay,
                           &afsk_.m_peak, &afsk_.m_valley);
        float s_norm = agc(s_amp, afsk_.agc_fast_attack, afsk_.agc_slow_decay,
                           &afsk_.s_peak, &afsk_.s_valley);

        demod_out = m_norm - s_norm;
    }

    nudge_pll(demod_out);
}

void Demodulator::process_9600(float fsam) {
    push_sample(fsam, bb_.audio_buf, bb_.lp_filter_taps);
    float filtered = convolve(bb_.audio_buf, bb_.lp_filter, bb_.lp_filter_taps);

    float demod_out = agc(filtered, bb_.agc_fast_attack, bb_.agc_slow_decay,
                          &bb_.peak, &bb_.valley);

    nudge_pll(demod_out);
}

void Demodulator::nudge_pll(float demod_out) {
    prev_d_c_pll_ = data_clock_pll_;

    // Advance PLL (unsigned add to avoid signed overflow)
    data_clock_pll_ = (int)((unsigned int)data_clock_pll_ + (unsigned int)pll_step_per_sample_);

    // Count samples since last transition (for DCD silence detection)
    dcd_missing_++;

    // Overflow (positive → negative): sample a data bit
    if (data_clock_pll_ < 0 && prev_d_c_pll_ > 0) {
        int raw_bit = demod_out > 0 ? 1 : 0;

        int dbit;
        if (type_ == GMSK_9600 || type_ == AIS) {
            int descrambled = descramble(raw_bit, &bb_.scramble_state);
            dbit = (descrambled == prev_demod_bit_) ? 1 : 0;
            prev_demod_bit_ = descrambled;
        } else {
            dbit = (raw_bit == prev_demod_bit_) ? 1 : 0;
            prev_demod_bit_ = raw_bit;
        }

        if (on_bit_) on_bit_(dbit);
    }

    // --- DCD tracking at every data transition ---
    int demod_data = demod_out > 0 ? 1 : 0;
    if (demod_data != prev_demod_data_) {
        // Transition detected — evaluate quality
        // A "good" transition has PLL phase near zero (mid-point between samples)
        // PLL is signed 32-bit: 0 = perfect mid-bit, ±2^31 = worst
        // Good = PLL magnitude < 25% of full cycle
        unsigned int pll_mag = (unsigned int)(data_clock_pll_ < 0 ? -data_clock_pll_ : data_clock_pll_);
        bool good = (pll_mag < 0x40000000u);  // < 25% of 2^32

        // Shift into 32-bit quality register
        dcd_shreg_ <<= 1;
        if (good) dcd_shreg_ |= 1;

        // Popcount (count good transitions in last 32)
        unsigned int v = dcd_shreg_;
        int cnt = 0;
        while (v) { cnt += (v & 1); v >>= 1; }
        dcd_count_ = cnt;

        // Hysteresis: DCD ON at >= 25/32, OFF at < 10/32
        if (!dcd_ && dcd_count_ >= DCD_THRESH_ON)
            dcd_ = true;
        else if (dcd_ && dcd_count_ < DCD_THRESH_OFF)
            dcd_ = false;

        dcd_missing_ = 0;

        // PLL nudge
        if (dcd_)
            data_clock_pll_ = (int)(data_clock_pll_ * pll_locked_inertia_);
        else
            data_clock_pll_ = (int)(data_clock_pll_ * pll_searching_inertia_);
    }

    // No transitions for too long → DCD off (silence / no signal)
    // Threshold: 4 bit periods worth of samples with no transition
    int silence_thresh = (sample_rate_ * 4) / (type_ == GMSK_9600 ? 9600 :
                          type_ == AFSK_300 ? 300 : 1200);
    if (dcd_missing_ > silence_thresh) {
        dcd_ = false;
        dcd_shreg_ = 0;
        dcd_count_ = 0;
    }

    prev_demod_data_ = demod_data;
}

// ===========================================================================
//  Modulator
// ===========================================================================

void Modulator::init_cos_table() {
    for (int j = 0; j < 256; j++)
        cos256_[j] = cosf((float)j * 2.0f * (float)M_PI / 256.0f);
}

void Modulator::init(Type type, int sample_rate, int amplitude) {
    type_ = type;
    sample_rate_ = sample_rate;
    amplitude_ = amplitude;
    osc_phase_ = 0;
    nrzi_state_ = 0;
    scramble_state_ = 0;
    current_bit_ = 0;

    init_cos_table();

    switch (type) {
        case AFSK_1200:
            mark_delta_ = (unsigned int)round(pow(2.0, 32.0) * 1200.0 / sample_rate);
            space_delta_ = (unsigned int)round(pow(2.0, 32.0) * 2200.0 / sample_rate);
            samples_per_bit_ = sample_rate / 1200;
            break;
        case AFSK_300:
            mark_delta_ = (unsigned int)round(pow(2.0, 32.0) * 1600.0 / sample_rate);
            space_delta_ = (unsigned int)round(pow(2.0, 32.0) * 1800.0 / sample_rate);
            samples_per_bit_ = sample_rate / 300;
            break;
        case GMSK_9600:
        case AIS:
            samples_per_bit_ = sample_rate / 9600;
            break;
        case EAS:
            mark_delta_ = (unsigned int)round(pow(2.0, 32.0) * 2083.0 / sample_rate);
            space_delta_ = (unsigned int)round(pow(2.0, 32.0) * 1563.0 / sample_rate);
            samples_per_bit_ = sample_rate / 521;
            break;
        default:
            mark_delta_ = (unsigned int)round(pow(2.0, 32.0) * 1200.0 / sample_rate);
            space_delta_ = (unsigned int)round(pow(2.0, 32.0) * 2200.0 / sample_rate);
            samples_per_bit_ = sample_rate / 1200;
            break;
    }
}

void Modulator::set_type(Type type) {
    init(type, sample_rate_, amplitude_);
}

int Modulator::scramble(int in, int* state) {
    int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
    *state = (*state << 1) | (out & 1);  // Note: scrambler feeds back the OUTPUT
    return out;
}

void Modulator::put_bit(int bit) {
    switch (type_) {
        case AFSK_1200:
        case AFSK_300:
        case EAS:
            put_afsk_bit(bit);
            break;
        case GMSK_9600:
        case AIS:
            put_9600_bit(bit);
            break;
        default:
            put_afsk_bit(bit);
            break;
    }
}

void Modulator::put_afsk_bit(int bit) {
    // NRZI: 0 = change frequency, 1 = keep current
    // (On air: mark=1=no change, space=0=change)
    // bit here is already NRZI-encoded by the HDLC encoder
    unsigned int delta = bit ? mark_delta_ : space_delta_;

    for (int i = 0; i < samples_per_bit_; i++) {
        // Continuous-phase FSK: just change the delta
        float s = cos256_[(osc_phase_ >> 24) & 0xff];
        int16_t sample = (int16_t)(s * amplitude_);
        if (on_sample_) on_sample_(sample);
        osc_phase_ += delta;
    }
}

void Modulator::put_9600_bit(int bit) {
    // `bit` is already NRZI-encoded by the HDLC encoder — just scramble and output
    int scrambled = scramble(bit, &scramble_state_);
    int16_t level = scrambled ? (int16_t)amplitude_ : (int16_t)(-amplitude_);

    for (int i = 0; i < samples_per_bit_; i++) {
        if (on_sample_) on_sample_(level);
    }
}

void Modulator::put_quiet_ms(int ms) {
    int n = sample_rate_ * ms / 1000;
    for (int i = 0; i < n; i++)
        if (on_sample_) on_sample_(0);
}

} // namespace modem
