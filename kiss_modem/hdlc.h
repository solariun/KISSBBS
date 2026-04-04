// HDLC framing for AX.25: bit stuffing, FCS/CRC16-CCITT, frame assembly
// FCS algorithm derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Simplified for kiss_modem: single-channel, C++ classes, no global state
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace hdlc {

// ---------------------------------------------------------------------------
//  CRC16-CCITT (FCS) — same polynomial & table as Direwolf / AX.25 spec
// ---------------------------------------------------------------------------
uint16_t fcs_calc(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
//  HDLC Decoder — bit-stream to AX.25 frames
// ---------------------------------------------------------------------------
//  Feed demodulated bits one at a time.  When a complete frame with valid
//  FCS is assembled the on_frame callback fires with the raw AX.25 bytes
//  (FCS stripped).
//
//  Implements: flag (0x7E) detection, NRZI decode, bit-unstuffing, FCS check.

class Decoder {
public:
    using FrameCb = std::function<void(const uint8_t* data, size_t len)>;

    void init();

    // Feed one demodulated bit (after NRZI decode).
    // `raw` is the NRZI-decoded bit value (0 or 1).
    void receive_bit(int raw);

    void set_on_frame(FrameCb cb) { on_frame_ = cb; }
    void set_debug(int level) { debug_ = level; }

private:
    static constexpr int MAX_FRAME = 330;  // 256 info + 14 addr + ctrl + pid + 2 FCS + margin
    static constexpr uint8_t FLAG = 0x7E;

    FrameCb on_frame_;
    int     debug_ = 0;   // 0=off, 3=verbose (FCS fail, abort)

    uint8_t pat_det_;          // 8-bit shift register for flag detection
    int     olen_;             // output bit counter within current byte
    uint8_t oacc_;             // output byte accumulator
    int     frame_len_;        // bytes assembled so far
    uint8_t frame_buf_[MAX_FRAME];
    int     consecutive_ones_; // for bit-unstuffing
    bool    collecting_;       // true after opening flag detected
};

// ---------------------------------------------------------------------------
//  HDLC Encoder — AX.25 frame to bit-stream
// ---------------------------------------------------------------------------
//  Given a raw AX.25 frame (without FCS), produces the full HDLC bit-stream:
//    opening flags | data (bit-stuffed, NRZI) | FCS (bit-stuffed, NRZI) | closing flag
//
//  Call encode() to get a vector of audio-ready bits, or use send_bit callback
//  for streaming.

class Encoder {
public:
    using BitCb = std::function<void(int bit)>;

    void set_on_bit(BitCb cb) { on_bit_ = cb; }

    // Encode a complete frame.  Calls on_bit for each output bit.
    // `preamble_flags` = number of 0x7E flags before data (typ. 25-40 for 300ms @ 1200 baud)
    // `postamble_flags` = trailing flags (typ. 2)
    void send_frame(const uint8_t* data, size_t len, int preamble_flags = 30, int postamble_flags = 2);

    // Convenience: returns all bits as a vector
    std::vector<int> encode(const uint8_t* data, size_t len, int preamble_flags = 30, int postamble_flags = 2);

private:
    BitCb on_bit_;
    int   nrzi_state_ = 0;   // current NRZI output level
    int   ones_count_ = 0;   // consecutive 1s for bit-stuffing

    void send_flag();
    void send_byte_nrzi(uint8_t byte, bool stuff);
    void send_bit_nrzi(int bit);
    void send_bit_raw(int bit);   // after NRZI: calls on_bit_
};

} // namespace hdlc
