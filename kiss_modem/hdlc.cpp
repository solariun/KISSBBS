// HDLC framing for AX.25: bit stuffing, FCS/CRC16-CCITT, frame assembly
// FCS table and algorithm derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Original: https://github.com/wb2osz/direwolf
// Simplified for kiss_modem: single-channel, C++ classes, no global state
#include "hdlc.h"
#include <cstring>

namespace hdlc {

// ---------------------------------------------------------------------------
//  CRC16-CCITT table — from RFC 1549 via Direwolf fcs_calc.c
// ---------------------------------------------------------------------------
static const uint16_t ccitt_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

uint16_t fcs_calc(const uint8_t* data, size_t len) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ ccitt_table[(crc ^ data[i]) & 0xff];
    return crc ^ 0xffff;
}

// ---------------------------------------------------------------------------
//  Decoder
// ---------------------------------------------------------------------------

void Decoder::init() {
    pat_det_ = 0;
    olen_ = 0;
    oacc_ = 0;
    frame_len_ = 0;
    consecutive_ones_ = 0;
    collecting_ = false;
}

void Decoder::receive_bit(int raw) {
    int dbit = raw & 1;

    // Shift into 8-bit pattern detector (LSB first)
    pat_det_ >>= 1;
    if (dbit) pat_det_ |= 0x80;

    // ---- Flag detection (0x7E = 01111110) ----
    if (pat_det_ == FLAG) {
        // If we were collecting and have enough data (min AX.25 = 15 bytes + 2 FCS)
        if (collecting_ && frame_len_ >= 17) {
            // Check FCS: last 2 bytes are the received FCS
            uint16_t received_fcs = (uint16_t)frame_buf_[frame_len_ - 1] << 8
                                  | (uint16_t)frame_buf_[frame_len_ - 2];
            uint16_t computed_fcs = fcs_calc(frame_buf_, frame_len_ - 2);
            if (received_fcs == computed_fcs) {
                if (on_frame_) on_frame_(frame_buf_, frame_len_ - 2);
            } else if (debug_ >= 3) {
                fprintf(stderr, "  [HDLC] FCS fail: %d bytes (got=%04x want=%04x)\n",
                        frame_len_ - 2, received_fcs, computed_fcs);
            }
        }
        // Reset for next frame
        collecting_ = true;
        olen_ = 0;
        oacc_ = 0;
        frame_len_ = 0;
        consecutive_ones_ = 0;
        return;
    }

    // ---- Abort detection: 7+ consecutive 1s ----
    if (dbit) {
        consecutive_ones_++;
        if (consecutive_ones_ >= 7) {
            if (debug_ >= 3 && collecting_ && frame_len_ > 0)
                fprintf(stderr, "  [HDLC] abort at %d bytes\n", frame_len_);
            collecting_ = false;
            frame_len_ = 0;
            olen_ = 0;
            return;
        }
    }

    if (!collecting_) {
        if (!dbit) consecutive_ones_ = 0;
        return;
    }

    // ---- Bit-unstuffing: after 5 ones, skip stuffed 0 ----
    if (dbit) {
        // Still accumulating ones
    } else {
        if (consecutive_ones_ == 5) {
            // This 0 was stuffed — discard it
            consecutive_ones_ = 0;
            return;
        }
        consecutive_ones_ = 0;
    }

    // ---- Accumulate data bits (LSB first) ----
    oacc_ >>= 1;
    if (dbit) oacc_ |= 0x80;
    olen_++;

    if (olen_ >= 8) {
        if (frame_len_ < MAX_FRAME) {
            frame_buf_[frame_len_++] = oacc_;
        } else {
            // Frame too long — abort
            collecting_ = false;
            frame_len_ = 0;
        }
        olen_ = 0;
        oacc_ = 0;
    }
}

// ---------------------------------------------------------------------------
//  Encoder
// ---------------------------------------------------------------------------

void Encoder::send_bit_raw(int bit) {
    if (on_bit_) on_bit_(bit);
}

// NRZI: 0 = transition (invert), 1 = no change
void Encoder::send_bit_nrzi(int bit) {
    if (bit == 0)
        nrzi_state_ = !nrzi_state_;
    // else: no change
    send_bit_raw(nrzi_state_);
}

void Encoder::send_flag() {
    // Flag 0x7E = 01111110 — sent without bit stuffing, LSB first
    // Reset ones count (flags break the stuffing context)
    ones_count_ = 0;
    uint8_t flag = 0x7E;
    for (int i = 0; i < 8; i++) {
        send_bit_nrzi((flag >> i) & 1);
    }
}

void Encoder::send_byte_nrzi(uint8_t byte, bool stuff) {
    for (int i = 0; i < 8; i++) {
        int bit = (byte >> i) & 1;
        send_bit_nrzi(bit);

        if (stuff) {
            if (bit) {
                ones_count_++;
                if (ones_count_ == 5) {
                    // Insert stuffed 0
                    send_bit_nrzi(0);
                    ones_count_ = 0;
                }
            } else {
                ones_count_ = 0;
            }
        }
    }
}

void Encoder::send_frame(const uint8_t* data, size_t len, int preamble_flags, int postamble_flags) {
    nrzi_state_ = 0;
    ones_count_ = 0;

    // Preamble flags
    for (int i = 0; i < preamble_flags; i++)
        send_flag();

    // Data bytes with bit stuffing
    ones_count_ = 0;
    for (size_t i = 0; i < len; i++)
        send_byte_nrzi(data[i], true);

    // FCS (sent LSB first, with bit stuffing)
    uint16_t fcs = fcs_calc(data, len);
    send_byte_nrzi(fcs & 0xff, true);
    send_byte_nrzi((fcs >> 8) & 0xff, true);

    // Postamble flags
    for (int i = 0; i < postamble_flags; i++)
        send_flag();
}

std::vector<int> Encoder::encode(const uint8_t* data, size_t len, int preamble_flags, int postamble_flags) {
    std::vector<int> bits;
    bits.reserve((preamble_flags + postamble_flags) * 8 + (len + 2) * 10);
    auto prev_cb = on_bit_;
    on_bit_ = [&bits](int b) { bits.push_back(b); };
    send_frame(data, len, preamble_flags, postamble_flags);
    on_bit_ = prev_cb;
    return bits;
}

} // namespace hdlc
