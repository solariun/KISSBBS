// Modem demodulator — AFSK 1200/300, GMSK 9600, PSK 2400/4800
// DSP algorithms derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Original: https://github.com/wb2osz/direwolf
// Simplified for modemtnc: single-decoder, C++ classes, no global state
#pragma once
#include <cstdint>
#include <functional>

namespace modem {

enum Type {
    AFSK_1200,   // Bell 202: mark=1200 Hz, space=2200 Hz, 1200 baud
    AFSK_300,    // HF: mark=1600 Hz, space=1800 Hz, 300 baud
    GMSK_9600,   // G3RUH scrambled baseband, 9600 baud
    PSK_2400,    // QPSK V.26B, 2400 bps
    PSK_4800,    // 8PSK, 4800 bps
    AIS,         // 9600 baud baseband (maritime)
    EAS,         // AFSK 521 baud (Emergency Alert)
};

// Maximum FIR filter taps
static constexpr int MAX_FILTER = 480;

// ---------------------------------------------------------------------------
//  Demodulator
// ---------------------------------------------------------------------------
class Demodulator {
public:
    using BitCb = std::function<void(int bit)>;

    void init(Type type, int sample_rate);
    void process_sample(int16_t sample);
    void set_on_bit(BitCb cb) { on_bit_ = cb; }
    void set_type(Type type);   // runtime switch (re-initializes filters)
    bool dcd() const { return dcd_; }
    int  sample_rate() const { return sample_rate_; }

private:
    BitCb on_bit_;
    Type  type_ = AFSK_1200;
    int   sample_rate_ = 44100;
    bool  dcd_ = false;

    // ----- AFSK state -----
    struct {
        // Local oscillators (DDS)
        unsigned int m_osc_phase, m_osc_delta;  // mark
        unsigned int s_osc_phase, s_osc_delta;  // space

        // I/Q sample buffers for mark and space
        float m_I_raw[MAX_FILTER];
        float m_Q_raw[MAX_FILTER];
        float s_I_raw[MAX_FILTER];
        float s_Q_raw[MAX_FILTER];

        // Pre-filter (bandpass)
        float raw_cb[MAX_FILTER];
        float pre_filter[MAX_FILTER];
        int   pre_filter_taps;
        bool  use_prefilter;

        // Low-pass / RRC filter
        float lp_filter[MAX_FILTER];
        int   lp_filter_taps;

        // AGC
        float agc_fast_attack, agc_slow_decay;
        float m_peak, m_valley;
        float s_peak, s_valley;
    } afsk_;

    // ----- 9600 baud state -----
    struct {
        float audio_buf[MAX_FILTER];
        float lp_filter[MAX_FILTER];
        int   lp_filter_taps;
        int   scramble_state;

        // AGC
        float agc_fast_attack, agc_slow_decay;
        float peak, valley;
    } bb_;

    // ----- PLL (shared) -----
    int   pll_step_per_sample_;
    int   data_clock_pll_;
    int   prev_d_c_pll_;
    float pll_locked_inertia_;
    float pll_searching_inertia_;
    int   prev_demod_data_;     // per-sample: for PLL transition detection
    int   prev_demod_bit_;      // per-bit: for NRZI decode at sample points

    // ----- DCD tracking -----
    unsigned int dcd_shreg_;     // 32-bit shift register for quality tracking
    int          dcd_count_;

    // Cosine table
    float cos256_[256];

    // Helpers
    void init_afsk(int baud, int mark, int space);
    void init_9600();
    void process_afsk(float fsam);
    void process_9600(float fsam);
    void nudge_pll(float demod_out);

    float fcos256(unsigned int phase) const { return cos256_[(phase >> 24) & 0xff]; }
    float fsin256(unsigned int phase) const { return cos256_[((phase >> 24) - 64) & 0xff]; }

    // DSP primitives
    static void  push_sample(float val, float* buf, int size);
    static float convolve(const float* data, const float* filter, int taps);
    static float agc(float in, float fast_attack, float slow_decay, float* peak, float* valley);
    static int   descramble(int in, int* state);

    // Filter generation
    static void gen_bandpass(float f1, float f2, float* filter, int taps);
    static void gen_lowpass(float fc, float* filter, int taps);
    static void gen_rrc_lowpass(float* filter, int taps, float rolloff, float samples_per_symbol);
};

// ---------------------------------------------------------------------------
//  Modulator
// ---------------------------------------------------------------------------
class Modulator {
public:
    using SampleCb = std::function<void(int16_t sample)>;

    void init(Type type, int sample_rate, int amplitude = 16000);
    void set_on_sample(SampleCb cb) { on_sample_ = cb; }
    void set_type(Type type);

    // Modulate one bit (called by HDLC encoder)
    void put_bit(int bit);

    // Generate silence (ms)
    void put_quiet_ms(int ms);

    bool is_busy() const { return false; }  // TODO: TX queue

private:
    SampleCb on_sample_;
    Type type_ = AFSK_1200;
    int  sample_rate_ = 44100;
    int  amplitude_ = 16000;

    // AFSK state
    unsigned int osc_phase_ = 0;
    unsigned int mark_delta_ = 0;
    unsigned int space_delta_ = 0;
    int samples_per_bit_ = 0;
    int current_bit_ = 0;

    // 9600 baud state
    int  nrzi_state_ = 0;
    int  scramble_state_ = 0;

    // Cosine table (shared with demod or separate instance)
    float cos256_[256];

    void init_cos_table();
    void put_afsk_bit(int bit);
    void put_9600_bit(int bit);
    static int scramble(int in, int* state);
};

} // namespace modem
