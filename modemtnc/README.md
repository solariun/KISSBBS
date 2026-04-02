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

## Selecting an Audio Device

Before running modemtnc, identify which audio device to use:

```bash
./bin/modemtnc --list-devices
```

### macOS output example

```
#     Device Name                               IN     OUT    UID
---   ----------------------------------------  -----  -----  ---
0     MacBook Air Microphone                    yes    -      BuiltInMicrophoneDevice
1     MacBook Air Speakers                      -      yes    BuiltInSpeakerDevice
2     SignaLink USB Audio CODEC                  yes    yes    AppleUSBAudioEngine:...
```

Look for your sound interface (SignaLink, DRAWS, DINAH, etc.) — it needs both
**IN** (capture from radio) and **OUT** (transmit to radio).

### Linux output example

```
#     ALSA Device           Description                               IN     OUT
---   --------------------  ----------------------------------------  -----  -----
0     default               Default Audio Device                      yes    yes
1     sysdefault:CARD=...   USB Audio CODEC, USB Audio Analog         yes    yes
2     hw:1,0                USB Audio CODEC                           yes    yes

Hardware cards:
  hw:0  HDA Intel PCH
  hw:1  USB Audio CODEC
```

For Linux, use the ALSA device name with `-d`:
```bash
# System default (usually works)
./bin/modemtnc -d default -s 1200 --link /tmp/kiss --monitor

# Specific USB sound card (card 1) with auto-conversion
./bin/modemtnc -d plughw:1,0 -s 1200 --link /tmp/kiss --monitor

# Direct hardware access (card 1) — lowest latency
./bin/modemtnc -d hw:1,0 -s 1200 --link /tmp/kiss --monitor
```

### Which device to choose?

| Scenario | macOS | Linux |
|----------|-------|-------|
| USB sound interface (SignaLink, DINAH) | Shows as extra device with IN+OUT | Use `plughw:N,0` where N is card number |
| Built-in soundcard | `MacBook Air Microphone` / `Speakers` | `default` or `hw:0,0` |
| Raspberry Pi HAT (DRAWS) | N/A | `plughw:1,0` (usually card 1) |
| Virtual audio (for SDR) | Use BlackHole or Loopback | Use `snd-aloop` module |

**Tip**: if unsure, run `--list-devices`, plug/unplug your interface, run again,
and compare — the new entry is your device.

## Command Line Reference

```
modemtnc [options]

Audio:
  -d DEVICE         Audio device name
                      Linux (ALSA): "default", "hw:1,0", "plughw:1,0"
                      macOS (CoreAudio): system default used automatically
  -r RATE           Sample rate in Hz (default: 44100)
                      Auto-selects 96000 for 9600 baud
  --list-devices    List all available audio devices and exit
                      Shows device name, IN/OUT capabilities, and UID/card info

Modem:
  -s SPEED          Baud rate (default: 1200)
                      300   — HF AFSK (1600/1800 Hz, SSB)
                      1200  — VHF/UHF AFSK Bell 202 (1200/2200 Hz)
                      9600  — UHF GMSK/G3RUH scrambled baseband
  --volume N        TX audio amplitude 0-100 (default: 50)
                      Adjust to avoid clipping in the radio

KISS interface:
  --link PATH       PTY symlink path (default: /tmp/kiss)
                      Any KISS client connects via this path
  --server-port N   TCP KISS server port (disabled by default)
                      Allows remote/network KISS clients

TX timing (CSMA/CA):
  --txdelay N       Preamble flags before data, in ms (default: 300)
                      Gives the remote receiver time to lock PLL
  --txtail N        Silence after last frame, in ms (default: 100)
  --persist N       CSMA persistence 0-255 (default: 63)
                      Higher = more aggressive channel access
  --slottime N      CSMA slot time in ms (default: 100)

Display:
  -c CALL           Your callsign (shown in monitor output headers)
  --monitor         Print all decoded frames to stdout with timestamps
                      Shows direction (<- AIR for RX, -> AIR for TX),
                      AX.25 decode, and hex dump

Testing:
  --loopback        Self-test: TX -> modulate -> demodulate -> RX in memory
                      No audio device needed — validates entire DSP chain
  -h, --help        Show help and exit
```

### Quick Command Examples

```bash
# List audio devices
modemtnc --list-devices

# Self-test (no hardware)
modemtnc --loopback --monitor

# Standard VHF packet (1200 baud AFSK)
modemtnc -d plughw:1,0 -s 1200 --link /tmp/kiss --monitor

# HF packet (300 baud AFSK via SSB)
modemtnc -d plughw:1,0 -s 300 --link /tmp/kiss --monitor

# High-speed UHF (9600 baud G3RUH)
modemtnc -d plughw:1,0 -s 9600 --link /tmp/kiss --monitor

# With TCP server for remote clients
modemtnc -d default -s 1200 --link /tmp/kiss --server-port 8001 --monitor

# Adjust TX timing for long-distance HF
modemtnc -d plughw:1,0 -s 300 --txdelay 500 --txtail 200 --link /tmp/kiss --monitor

# Connect clients to the TNC
ax25tnc -c W1AW -r W1BBS /tmp/kiss           # interactive terminal
ax25send -c W1AW /tmp/kiss --pos 42.36,-71.06 "Boston"   # APRS beacon
bbs -c W1BBS /tmp/kiss                        # BBS server
```

## Examples

### Loopback Self-Test (no hardware)

```bash
./bin/modemtnc --loopback --monitor -c TEST           # 1200 baud AFSK
./bin/modemtnc --loopback --monitor -c TEST -s 300    # 300 baud HF
./bin/modemtnc --loopback --monitor -c TEST -s 9600   # 9600 baud G3RUH
```

### Digirig + Any VHF/UHF Radio (1200 baud)

The [Digirig](https://digirig.net/) is a USB sound card with built-in
CM108 GPIO for PTT. Connect the Digirig to the radio's mic/speaker
or data port, and plug USB into the computer.

```bash
# 1. Find the audio device
./bin/modemtnc --list-devices
# Look for "USB Audio CODEC" or "Digirig" — note the ALSA device (e.g., plughw:1,0)

# 2. Run with CM108 PTT (auto-detects /dev/hidrawN)
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt cm108 --link /tmp/kiss --monitor

# 3. Or specify the HID device explicitly
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt cm108 --ptt-device /dev/hidraw0 --link /tmp/kiss --monitor

# 4. Connect clients
./bin/ax25tnc -c YOURCALL -r REMOTE /tmp/kiss              # interactive
./bin/ax25send -c YOURCALL /tmp/kiss --pos 42.36,-71.06 "Mobile"   # APRS beacon
```

**Digirig PTT notes:**
- The Digirig uses CM108 GPIO pin 3 (default) for PTT
- If PTT does not key: check `ls -la /dev/hidraw*` permissions
- Add udev rule: `echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d8c", GROUP="audio", MODE="0660"' | sudo tee /etc/udev/rules.d/99-cmedia.rules`

### Icom IC-7300MKII (HF, 300 or 1200 baud)

The IC-7300 has a built-in USB audio codec and USB serial port. No external
sound card needed — plug the USB cable directly.

```bash
# 1. Find audio and serial devices
./bin/modemtnc --list-devices
# Audio: look for "CODEC" or "IC-7300" (e.g., plughw:2,0 on Linux)
# Serial: /dev/ttyUSB0 or /dev/cu.usbmodem* (for CAT/PTT)

# 2. HF packet at 300 baud with serial RTS PTT
./bin/modemtnc -d plughw:2,0 -s 300 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --txdelay 500 --link /tmp/kiss --monitor

# 3. VHF/UHF packet at 1200 baud
./bin/modemtnc -d plughw:2,0 -s 1200 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --link /tmp/kiss --monitor

# macOS: use cu.* device names
./bin/modemtnc -s 300 \
    --ptt rts --ptt-device /dev/cu.usbmodem14201 \
    --txdelay 500 --link /tmp/kiss --monitor
```

**IC-7300 radio settings:**
- **Menu > Set > Connectors > USB AF Output Level**: adjust for good audio level (not clipping)
- **Menu > Set > Connectors > USB AF/IF Output**: AF (audio frequency)
- **Menu > Set > Connectors > USB Send**: off (we control PTT via RTS, not USB CI-V)
- **Menu > Set > Connectors > CI-V USB Port**: unlink from PTT if using RTS
- **Mode**: USB/LSB for 300 baud HF packet (SSB mode, not FM)
- **Data mode**: DATA OFF (modemtnc handles the modem, not the radio)

### Yaesu FT-991A (HF + VHF/UHF, 300/1200/9600 baud)

The FT-991A has USB audio and USB serial built-in (similar to IC-7300).

```bash
# 1. Find devices
./bin/modemtnc --list-devices
# Audio: "USB Audio CODEC" (e.g., plughw:1,0)
# Serial: /dev/ttyUSB0 (CAT) — there are TWO serial ports, use the CAT one

# 2. HF 300 baud packet (SSB mode on the radio)
./bin/modemtnc -d plughw:1,0 -s 300 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --txdelay 500 --link /tmp/kiss --monitor

# 3. VHF 1200 baud AFSK
./bin/modemtnc -d plughw:1,0 -s 1200 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --link /tmp/kiss --monitor

# 4. UHF 9600 baud (requires flat audio — use DATA connector or 9600 baud jack)
./bin/modemtnc -d plughw:1,0 -s 9600 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --link /tmp/kiss --monitor

# macOS
./bin/modemtnc -s 1200 \
    --ptt rts --ptt-device /dev/cu.usbserial-* \
    --link /tmp/kiss --monitor
```

**FT-991A radio settings:**
- **Menu 037 CAT SELECT**: USB
- **Menu 038 CAT RATE**: 38400 (match with system)
- **Menu 064 DATA MODE**: OTHERS (not PSK/RTTY — modemtnc is the modem)
- **Menu 065 OTHER DISP (SSB)**: 3000 Hz
- **Menu 070 DATA OUT LEVEL**: adjust (start at 50)
- **Menu 071 DATA IN LEVEL**: adjust (start at 50)
- **Mode**: USB-D for HF packet, FM for VHF/UHF
- For 9600 baud: use the mini-DIN DATA connector, not the mic jack

### Digirig + IC-7300 or FT-991A

You can also use a Digirig between the radio's audio jack and the computer,
instead of the built-in USB audio. This is useful if the radio's USB audio
has issues or you want to use the USB port for CAT control only.

```bash
# Digirig on audio, radio USB for PTT
./bin/modemtnc -d plughw:1,0 -s 1200 \
    --ptt rts --ptt-device /dev/ttyUSB0 \
    --link /tmp/kiss --monitor

# Or Digirig with its own CM108 PTT (no radio serial needed)
./bin/modemtnc -d plughw:1,0 -s 1200 \
    --ptt cm108 --link /tmp/kiss --monitor
```

### TCP KISS Server

```bash
# Expose KISS via TCP for network clients
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt cm108 --server-port 8001 --monitor

# Connect remotely
./bin/ax25tnc -c W1AW -r W1BBS localhost:8001
```

### Generic VOX Setup

If your interface has built-in VOX (some SignaLink USB configurations):

```bash
# No --ptt flag needed (defaults to VOX)
./bin/modemtnc -d plughw:1,0 -s 1200 --link /tmp/kiss --monitor
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

## PTT (Push-To-Talk) Control

Without PTT control, the radio will not transmit. modemtnc supports multiple
PTT methods to work with any radio setup:

| Method | Flag | How it works | Common use |
|--------|------|-------------|------------|
| **VOX** | `--ptt vox` (default) | Audio level triggers the radio's VOX circuit | SignaLink USB (turn DLY fully CCW) |
| **Serial RTS** | `--ptt rts` or `--ptt +rts` | Asserts RTS line on a serial port | IC-7300, FT-991A (USB serial), homebrew cables |
| **Serial RTS (inv)** | `--ptt -rts` | Asserts RTS inverted (active low) | Interfaces with inverted logic |
| **Serial DTR** | `--ptt dtr` or `--ptt +dtr` | Asserts DTR line on a serial port | Some homebrew interfaces |
| **Serial DTR (inv)** | `--ptt -dtr` | Asserts DTR inverted (active low) | Interfaces with inverted logic |
| **CM108 GPIO** | `--ptt cm108` | Sets GPIO pin on CM108/CM119 USB audio chip | Digirig, cheap USB sound cards |
| **GPIO (sysfs)** | `--ptt gpio` | Writes to /sys/class/gpio (Raspberry Pi, etc.) | DRAWS HAT, custom GPIO wiring |
| **CAT (hamlib)** | `--ptt hamlib` | Computer Aided Transceiver via hamlib | Any radio with CAT (future) |

The `+`/`-` prefix follows the Direwolf convention:
`+rts` = active high (default), `-rts` = active low (inverted).
You can also use `--ptt-invert` separately with any method.

### PTT Timing

```
              txdelay                  frame data              txtail
    ├─────────────────────┤├──────────────────────────┤├──────────────┤
PTT ─┐                                                                ┌── OFF
     └────────────────────────────────────────────────────────────────┘
     ON                        transmitting audio                    OFF
```

- `--txdelay N` (ms): silence before data — gives the remote receiver time to
  open squelch and the PLL to lock. Default 300 ms (45 flags at 1200 baud).
- `--txtail N` (ms): silence after data — ensures the last bit is fully
  transmitted before PTT drops. Default 100 ms.

### CM108/CM119 (Digirig) Setup

The Digirig and many cheap USB audio interfaces use a CM108 or CM119 chip
that has GPIO pins accessible via HID. modemtnc auto-detects the HID device.

```bash
# Check if a CM108/CM119 device is present
ls -la /dev/hidraw*

# If permission denied, add udev rule:
echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d8c", GROUP="audio", MODE="0660"' \
    | sudo tee /etc/udev/rules.d/99-cmedia.rules
sudo udevadm control --reload-rules && sudo udevadm trigger

# Verify your user is in the audio group
groups $USER   # should include 'audio'
sudo usermod -aG audio $USER   # if not, add and re-login
```

### Serial RTS/DTR Setup

For radios with USB serial (IC-7300, FT-991A) or external serial adapters:

```bash
# Linux: find the serial port
ls /dev/ttyUSB* /dev/ttyACM*

# macOS: find the serial port
ls /dev/cu.usbserial* /dev/cu.usbmodem*

# Use RTS for PTT
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt rts --ptt-device /dev/ttyUSB0 --monitor

# If PTT is inverted (active low) — two equivalent ways:
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt -rts --ptt-device /dev/ttyUSB0 --monitor
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt rts --ptt-device /dev/ttyUSB0 --ptt-invert --monitor
```

### Testing PTT — Step by Step

Before going on-air, verify that PTT activates correctly and audio is flowing.
Follow these steps in order:

**Step 1 — Loopback (no hardware)**
```bash
./bin/modemtnc --loopback --monitor -c TEST
# Expected: "Result: PASS" — confirms DSP chain works
```

**Step 2 — List devices**
```bash
./bin/modemtnc --list-devices
# Note your audio device (e.g., plughw:1,0) and serial port (e.g., /dev/ttyUSB0)
```

**Step 3 — Start modemtnc with PTT and monitor**
```bash
# Terminal 1: start the TNC
# Digirig:
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt cm108 --link /tmp/kiss --monitor

# IC-7300 / FT-991A:
./bin/modemtnc -d plughw:1,0 -s 1200 --ptt rts --ptt-device /dev/ttyUSB0 --link /tmp/kiss --monitor
```

You should see:
```
  PTT: CM108 GPIO 3 on /dev/hidraw0
```
or:
```
  PTT: Serial RTS on /dev/ttyUSB0
```

**Step 4 — Send a test UI frame**
```bash
# Terminal 2: send a test packet
./bin/ax25send -c YOURCALL /tmp/kiss --ui CQ "PTT test 1 2 3"
```

Watch Terminal 1 — you should see:
```
[HH:MM:SS.mmm]  -> AIR  YOURCALL>CQ [UI] PID=0xF0 | PTT test 1 2 3
```

And on the radio:
- **TX LED** lights up (PTT keyed)
- **Power meter** deflects (audio being transmitted)
- TX LED goes off after the frame

**Step 5 — Send an APRS position**
```bash
./bin/ax25send -c YOURCALL /tmp/kiss --pos 42.3601,-71.0589 "Testing modemtnc"
```

**Step 6 — Monitor for incoming packets**
```bash
# Leave modemtnc running — any packet received on-air will appear:
[HH:MM:SS.mmm]  <- AIR  W1AW>CQ [UI] PID=0xF0 | Hello from W1AW
```

### PTT Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| No TX LED on radio | PTT not keying | Check `--ptt` method and `--ptt-device` path |
| TX LED but no signal on SDR | Audio not reaching radio | Check audio device with `--list-devices`, verify cable |
| TX LED stays on forever | PTT stuck | Ctrl-C modemtnc (releases PTT on exit), check `--ptt-invert` |
| Permission denied on `/dev/hidraw*` | CM108 HID permissions | Add udev rule (see CM108 section above) |
| Permission denied on `/dev/ttyUSB*` | Serial port permissions | `sudo usermod -aG dialout $USER` then re-login |
| PTT keys but wrong polarity | Inverted PTT logic | Use `--ptt -rts` or `--ptt-invert` |
| "cannot open" serial device | Wrong device path | Run `ls /dev/ttyUSB* /dev/ttyACM*` to find the right one |
| PTT keys but no audio | Separate audio + PTT devices | Verify `-d` audio device matches your sound card, not PTT port |

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
