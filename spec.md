# AX25Toolkit — Spec

## kiss_modem — TX Queue & Audio Redesign

### Problem

1. Long RX packet → ACK never transmitted.
   - `wait_drain()` held `tx_mtx_` while bulk-copying samples into ring, blocking the
     CoreAudio real-time render callback.  macOS kills/resets a unit whose RT thread
     blocks too long → silent audio, PTT pulses but nothing plays.
   - `kp.txdelay` / `kp.txtail` (set by KISS clients over PTY or TCP) could override
     the modem's own timing, allowing remote peers to corrupt RF timing.

2. TX queue had no P/F-bit awareness.
   - Batching all queued frames into one PTT burst is wrong when a frame carries P=1
     (poll) — the remote must respond before the next burst.

3. No post-PTT cooldown — back-to-back PTT cycles with no guard time.

---

### Design

#### TX timing authority

`cfg.txdelay` and `cfg.txtail` (set via `--txdelay` / `--txtail` CLI flags) are the sole
timing authority.  KISS parameter commands (`TxDelay`, `TxTail`) from clients are
accepted and stored in `kp` for protocol compliance but **never used for TX timing**.

Minimum preamble: `max(cfg.txdelay × baud / 800, 15)` flags.

#### TX queue

`std::deque<std::vector<uint8_t>>` — unbounded FIFO, bounded only by RAM.
Frames are never dropped; clients (PTY or TCP) may enqueue at any time.

#### TX thread state machine

```
WAIT_WORK   cv.wait until queue non-empty
DCD_WAIT    poll demod.dcd() every 5ms until channel clear
SETTLE      20ms fixed (squelch tail)
BATCH       pop frames until queue empty OR P=1 frame reached (inclusive)
MODULATE    txdelay preamble + frame(s) + txtail silence
TX          PTT ON → write → wait_drain → PTT OFF
COOLDOWN    50ms hard guard — no re-key
            → queue non-empty? → DCD_WAIT (cv.wait predicate skips block)
            → queue empty?     → WAIT_WORK
```

#### P/F bit — not inspected

The modem is KISS-dumb.  It does not inspect AX.25 control bytes or manage any
AX.25 state (no T1, no window, no poll tracking).  All frames in the queue at
DCD-clear time are batched into one PTT burst regardless of content.
Upper-layer protocol correctness (P/F, window flow) is the application's responsibility.

#### AX.25 monitor

`--monitor` decodes and displays frames using only the same ax25lib subset as
bt_kiss_bridge: `ax25::Frame::decode()`, `frame.format()`, `ax25::kiss::encode()`,
`ax25::kiss::Decoder`, and `hex_dump()` from ax25dump.hpp.
No Connection, Router, or connected-mode AX.25 features are used.

#### Half-duplex RX mute (self-echo suppression)

Standard TNC behavior: the demodulator continues running during TX (to maintain DCD
state), but any frame decoded by HDLC while `tx_active` is true is silently dropped.
After PTT OFF, the HDLC decoder is reset (`init()`) to flush any partial frame
assembled from the echo.

This prevents the host AX.25 stack from seeing its own transmitted frames as received
frames, which would corrupt the connection state machine (duplicate ACKs, spurious
REJs, retransmission storms).

Platform-independent: the `tx_active` flag is in `kiss_modem.cpp`, applies to both
CoreAudio (macOS) and ALSA (Linux).

#### Audio — lock-free TX ring (macOS)

The CoreAudio render callback (`tx_callback`) is a real-time thread.  Holding a mutex
inside it risks priority inversion and CoreAudio unit reset.

Replaced `std::mutex tx_mtx_` + plain-int `tx_wr_/tx_rd_/tx_avail_` with an SPSC
lock-free ring using `std::atomic<int> tx_wr_` / `tx_rd_`.  Single producer
(`wait_drain`), single consumer (`tx_callback`) — no mutex required.

`wait_drain` drain strategy: 1ms-poll loop checking ring occupancy, hard timeout at
`(samples / out_rate × 1000) + 200ms`.  Stops as soon as ring empties.

---

#### Debug levels (`--debug N`)

All debug and monitor output is timestamped (`HH:MM:SS.mmm`).

| Level | Tags | Content |
|-------|------|---------|
| 1 | `[TX]` | PTT ON/OFF, burst frame count, sample count, audio duration |
| 2 | `[DCD]`, `[QUEUE]`, `[PTY]` | DCD wait/clear, queue depth, PTY/TCP raw hex |
| 3 | `[HDLC]` | FCS failures (got vs expected), abort events |

`--monitor` is independent of `--debug` and always prints decoded AX.25 frames.

---

### Files changed

| File | Change |
|------|--------|
| `kiss_modem/kiss_modem.cpp` | TX queue → deque; TX thread state machine; timing always from cfg; `--debug N` (1-3) with timestamps |
| `kiss_modem/audio_coreaudio.cpp` | Lock-free TX ring; remove tx_mtx_; wait_drain drain loop |
| `kiss_modem/hdlc.h` | `set_debug(bool)` → `set_debug(int)` for level support |
| `kiss_modem/hdlc.cpp` | Debug prints gated at level 3 |
| `kiss_modem/README.md` | `--debug N` documented in usage reference |
| `DESIGN.md` | New "Debug & Diagnostics" subsection in kiss_modem section |
