# modemtnc -- Software TNC with Soundcard DSP

A self-contained software TNC that replaces a hardware TNC by performing
AX.25 HDLC framing and modem DSP in software, using the computer's soundcard
as the radio interface.

DSP algorithms derived from **[Dire Wolf](https://github.com/wb2osz/direwolf)**
by John Langner, WB2OSZ (GPLv2).
Simplified: single-decoder architecture, C++ classes, no global state.

## Quick Start

```bash
# Build
make modemtnc

# Self-test (no audio device needed)
./bin/modemtnc --loopback --monitor

# Run with soundcard
./bin/modemtnc -s 1200 --link /tmp/kiss --monitor

# Connect any KISS client
./bin/ax25tnc -c W1AW -r W1BBS /tmp/kiss
```

## Supported Modems

| Mode | Baud | Modulation | Typical Use | Sample Rate |
|------|------|------------|-------------|-------------|
| AFSK 1200 | 1200 | Bell 202 (1200/2200 Hz) | VHF/UHF packet, APRS | 44100 Hz |
| AFSK 300 | 300 | 1600/1800 Hz | HF packet (SSB) | 44100 Hz |
| GMSK 9600 | 9600 | G3RUH scrambled baseband | High-speed UHF | 96000 Hz (auto) |

The modem type is selected at startup via `-s SPEED`. For 9600 baud, the
sample rate is automatically raised to 96000 Hz (override with `-r`).

## Architecture

```
  Soundcard (mic/line-in)          Soundcard (speaker/line-out)
         |                                  ^
         v                                  |
  +-------------+                  +---------------+
  | AudioDevice |                  | AudioDevice   |
  +------+------+                  +-------+-------+
         |                                  ^
    RX samples (16-bit PCM mono)       TX samples
         v                                  |
  +------+------+                  +-------+-------+
  |  Demodulator |  AFSK/GMSK     | Modulator      |
  +------+------+                  +-------+-------+
         |                                  ^
    raw bits                           raw bits
         v                                  |
  +------+------+                  +-------+-------+
  | HDLC Decoder |                 | HDLC Encoder   |
  +------+------+                  +-------+-------+
         |                                  ^
   AX.25 frames                     AX.25 frames
         v                                  |
  +------+------+                  +-------+-------+
  | KISS encode  |                 | KISS decode    |
  +------+------+                  +-------+-------+
         |                                  ^
         v                                  |
  +---------------------------------------------+
  |        PTY (/tmp/kiss) + TCP server          |
  +---------------------------------------------+
```

**RX path**: audio samples -> demodulate -> HDLC decode -> KISS encode -> PTY/TCP

**TX path**: PTY/TCP -> KISS decode -> HDLC encode -> modulate -> audio samples

## Command Line Reference

```
modemtnc [options]

Audio:
  -d DEVICE         Audio device name
                    Linux (ALSA): "default", "hw:1", "plughw:1,0"
                    macOS (CoreAudio): system default (device selection TBD)
  -r RATE           Sample rate in Hz (default: 44100, auto 96000 for 9600 baud)

Modem:
  -s SPEED          Baud rate: 300, 1200, 9600 (default: 1200)
  --volume N        TX amplitude 0-100 (default: 50)

KISS interface:
  --link PATH       PTY symlink path (default: /tmp/kiss)
  --server-port N   TCP KISS server port (disabled by default)

TX timing:
  --txdelay N       TX preamble delay in ms (default: 300)
  --txtail N        TX tail in ms (default: 100)
  --persist N       CSMA persistence 0-255 (default: 63)
  --slottime N      CSMA slot time in ms (default: 100)

Display:
  -c CALL           Callsign (shown in monitor output)
  --monitor         Print decoded frames to stdout

Testing:
  --loopback        Self-test: modulate -> demodulate in memory (no audio device)
  -h, --help        Show help
```

## Examples

### APRS Receive (VHF, 1200 baud)

```bash
# Start TNC on 1200 baud
./bin/modemtnc -s 1200 --link /tmp/kiss --monitor

# In another terminal, send APRS beacon
./bin/ax25send -c PU1ABC /tmp/kiss --pos -23.55,-46.63 "Mobile"
```

### High-Speed Packet (UHF, 9600 baud)

```bash
# 9600 baud auto-selects 96000 Hz sample rate
./bin/modemtnc -s 9600 --link /tmp/kiss --monitor

# Connect to a remote station
./bin/ax25tnc -c W1AW -r W1BBS /tmp/kiss
```

### HF Packet (300 baud)

```bash
./bin/modemtnc -s 300 --link /tmp/kiss --monitor
```

### TCP KISS Server

```bash
# Expose KISS via TCP for network clients
./bin/modemtnc -s 1200 --server-port 8001 --monitor

# Connect remotely
./bin/ax25tnc -c W1AW -r W1BBS localhost:8001
```

### Loopback Self-Test

```bash
# Quick sanity check -- no audio hardware needed
./bin/modemtnc --loopback --monitor -c TEST
./bin/modemtnc --loopback -c TEST -s 300
./bin/modemtnc --loopback -c TEST -s 9600
```

## Compatible Audio Interfaces

Any device that exposes a standard soundcard interface works:

| Interface | Connection | Notes |
|-----------|-----------|-------|
| SignaLink USB | USB sound card + radio cable | Most popular, plug and play |
| DRAWS | Raspberry Pi HAT | Dual-port, GPIO PTT |
| DINAH | USB | Supports HF and VHF |
| SHARI | USB | SA818-based hotspot |
| DMK URI | USB | Plug and play |
| Built-in mic/speaker | 3.5mm jack | For testing (not recommended for TX) |
| SDR (via audio pipe) | Virtual audio | Use with gqrx, SDR#, rtl_fm |

### SDR Integration

For receive-only with an RTL-SDR:

```bash
# Terminal 1: RTL-SDR to audio pipe
rtl_fm -f 144.39M -s 22050 - | sox -t raw -r 22050 -e signed -b 16 -c 1 - -t alsa default

# Terminal 2: modemtnc
./bin/modemtnc -s 1200 --link /tmp/kiss --monitor
```

## HDLC & FCS

The HDLC layer implements standard AX.25 framing:
- **Flag detection**: 0x7E (01111110)
- **Bit stuffing**: insert 0 after 5 consecutive 1s
- **NRZI**: 0 = transition, 1 = no change (encoded by HDLC, decoded by demodulator)
- **FCS**: CRC16-CCITT (same polynomial as AX.25 spec)
- **Frame validation**: minimum 17 bytes (14 addr + ctrl + 2 FCS), FCS check

## AFSK Demodulator (1200/300 baud)

The demodulator uses the proven Dire Wolf "Profile A" algorithm:

1. **Bandpass pre-filter** around mark/space frequencies
2. **I/Q mixing** with DDS local oscillators for mark and space
3. **RRC low-pass filter** on I/Q channels
4. **Envelope detection** via `hypot(I, Q)`
5. **AGC** (fast attack / slow decay) normalizes mark and space amplitudes
6. **Comparison**: mark amplitude > space amplitude = mark bit
7. **PLL clock recovery**: 32-bit phase accumulator, nudge at transitions

## 9600 Baud Baseband (GMSK/G3RUH)

For 9600 baud, the signal is a scrambled NRZ baseband:

1. **Low-pass filter** on audio input
2. **AGC** normalization
3. **PLL clock recovery** (same as AFSK)
4. **G3RUH descrambler**: LFSR taps at positions 16 and 11
5. **NRZI decode**: same = 1, different = 0

The scrambler polynomial `x^17 + x^12 + 1` provides DC balance and clock content
for the PLL, as specified in the G3RUH modem standard.

## Dependencies

**Zero external dependencies** -- system libraries only:

| Platform | Audio Library | Framework/Package |
|----------|--------------|-------------------|
| macOS | CoreAudio + AudioToolbox | Built-in (Xcode CLT) |
| Linux | ALSA | `libasound2-dev` (apt) / `alsa-lib-devel` (dnf) |

## Build

```bash
# macOS (automatic)
make modemtnc

# Linux (needs ALSA headers)
sudo apt install libasound2-dev   # Debian/Ubuntu
make modemtnc

# Build everything
make
```

## Testing & Debugging

### Loopback Self-Test

The `--loopback` flag runs a complete TX-RX round trip in memory without
any audio hardware. This validates the entire chain:
HDLC encode -> NRZI -> modulate -> demodulate -> PLL -> NRZI decode -> HDLC decode -> FCS check

```bash
# Test all supported baud rates
./bin/modemtnc --loopback --monitor -c TEST -s 1200
./bin/modemtnc --loopback --monitor -c TEST -s 300
./bin/modemtnc --loopback --monitor -c TEST -s 9600

# Expected output for each:
# Result: PASS
# Frames decoded: 1
```

If a loopback test fails, the problem is in the DSP/HDLC chain, not audio.

### Debug Checklist

When frames are not being decoded, check these in order:

| Check | What to verify | How |
|-------|---------------|-----|
| 1. Audio input | Signal is reaching the soundcard | `arecord -f S16_LE -r 44100 -c 1 test.wav` then play back |
| 2. Signal level | Not clipping, not too quiet | Check peak levels in `test.wav` -- aim for -6dB to -20dB |
| 3. Sample rate | Matches expected for baud rate | 44100 for 1200/300, 96000 for 9600 |
| 4. Frequency | Correct mark/space for AFSK | 1200 Hz mark / 2200 Hz space for Bell 202 |
| 5. NRZI | Proper encoding/decoding | Loopback test validates this |
| 6. Bit stuffing | Correct after 5 consecutive 1s | Loopback test validates this |
| 7. FCS | CRC16-CCITT matches | Loopback test validates this |
| 8. PTT timing | txdelay long enough for radio | Increase `--txdelay` (300-500ms typical) |

### Audio Signal Analysis

Record raw audio from the soundcard to verify signal quality:

```bash
# Linux: record 10 seconds of raw audio
arecord -f S16_LE -r 44100 -c 1 -d 10 capture.wav

# macOS: use QuickTime or sox
sox -d -r 44100 -c 1 -b 16 capture.wav trim 0 10

# Analyze with Audacity or sox:
sox capture.wav -n stat        # peak level, RMS
sox capture.wav -n spectrogram # frequency content
```

For AFSK 1200, you should see two dominant tones at 1200 Hz (mark) and
2200 Hz (space) in the spectrogram.

### Common Issues

**No frames decoded (1200 baud AFSK)**:
- Audio level too low: increase radio volume or interface gain
- Audio level clipping: reduce gain (clipping destroys the tones)
- Wrong audio device: verify with `arecord -l` (Linux) or Audio MIDI Setup (macOS)
- De-emphasis mismatch: some radios apply de-emphasis which attenuates the
  2200 Hz space tone. The AGC compensates, but extreme cases need adjustment.

**No frames decoded (9600 baud)**:
- Sample rate too low: must be >= 48000 Hz (96000 recommended). The auto-select
  handles this, but verify with `-r 96000` explicitly.
- Audio path filtering: 9600 baud requires flat response to ~4800 Hz.
  Radios with narrow audio filters (< 3 kHz) will not pass 9600 baud.
- Wrong radio mode: the radio must pass flat baseband audio, not discriminator
  output. Some radios need a hardware mod for 9600 baud.

**TX but no RX (remote station doesn't hear)**:
- txdelay too short: the remote radio needs time to open squelch. Try `--txdelay 500`.
- Volume too high/low: adjust `--volume` to avoid clipping in the radio.
- PTT not keyed: if using a SignaLink or similar, verify VOX is triggering.

**PLL not locking**:
- The PLL needs at least 5-10 preamble flags to lock. If you see partial frames,
  increase `--txdelay` on the transmitting side.
- For 9600 baud, the PLL is more sensitive to sample rate. Use 96000 Hz.

### KISS Parameter Tuning

KISS parameters can be set via the command line or by a KISS client:

| Parameter | CLI | KISS cmd | Default | Description |
|-----------|-----|----------|---------|-------------|
| TX Delay | `--txdelay` | 0x01 | 300 ms | Preamble before data (in 10ms units via KISS) |
| Persistence | `--persist` | 0x02 | 63 | CSMA p-persistence (0-255, higher = more aggressive) |
| Slot Time | `--slottime` | 0x03 | 100 ms | CSMA slot duration |
| TX Tail | `--txtail` | 0x04 | 100 ms | Silence after last frame |

For a simplex channel with multiple stations, use conservative CSMA:
`--persist 63 --slottime 100`. For a dedicated point-to-point link:
`--persist 255 --slottime 0`.

### Technical Details

**PLL Clock Recovery**:
- 32-bit phase accumulator (wraps at 2^32)
- Step per sample = 2^32 * baud / sample_rate
- Transitions nudge PLL toward zero crossing
- Locked inertia: 0.74 (slow adjustment when tracking)
- Searching inertia: 0.50 (fast adjustment when acquiring)

**AGC (Automatic Gain Control)**:
- Fast attack: 0.70 (quickly tracks amplitude increase)
- Slow decay: 0.000090 (slowly releases when signal drops)
- Normalizes to -0.5 to +0.5 range
- Compensates for mark/space amplitude imbalance (common with de-emphasis)

**FIR Filters**:
- Hamming window for all filters
- Pre-filter (bandpass): ~8 symbol-widths, attenuates out-of-band noise
- RRC (Root Raised Cosine) low-pass: rolloff=0.20, width=2.80 symbols
- Filter taps scale with sample_rate/baud (higher sample rate = more taps)

**G3RUH Scrambler**:
- LFSR polynomial: x^17 + x^12 + 1
- Scrambler feedback: output bit fed back to shift register
- Descrambler feedback: input bit fed back (self-synchronizing)
- Provides DC balance and clock transitions for PLL

## Credits

DSP algorithms (AFSK demodulator, G3RUH scrambler, FIR filter generation,
PLL clock recovery, AGC, HDLC framing) are derived from
**[Dire Wolf](https://github.com/wb2osz/direwolf)** by John Langner, WB2OSZ,
licensed under GPLv2.

The modemtnc implementation simplifies the original:
- Single decoder (vs. 9 parallel slicers)
- C++ classes with no global state (vs. C with static arrays)
- Single audio channel (vs. 6 channels across 3 soundcards)
- No FX.25/IL2P (standard AX.25 only)
