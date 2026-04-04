// CoreAudio backend for macOS — hybrid AudioQueue (capture) + AudioUnit (output)
//
// Lessons learned:
//   - AudioQueue handles sample rate conversion for USB devices (44100↔48000)
//   - AudioUnit AUHAL gives reliable output on USB devices
//   - AudioQueue output only plays the first burst on USB (broken for TNC use)
//   - Lock-free ring buffer is essential — mutex in audio callback drops samples
//   - USB devices report 48000/32-bit but AudioQueue converts to 44100/16-bit
//   - Output must use HW native rate (AudioUnit doesn't convert on USB)
//   - TX samples must be resampled if modem rate ≠ output HW rate
//
// Architecture:
//   Capture:  AudioQueue → lock-free ring buffer → read()
//   Output:   write() → TX ring buffer → AudioUnit render callback → speaker
//   Drain:    usleep for audio duration (Direwolf approach)
#ifdef __APPLE__
#include "audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <chrono>

static constexpr int RX_RING = 65536;   // ~1.5s at 44100
static constexpr int TX_RING = 131072;  // ~3s — holds full TX burst
static constexpr int AQ_BUFS = 4;       // AudioQueue capture buffers
static constexpr int AQ_FRAMES = 1024;  // frames per capture buffer

class CoreAudioDevice : public AudioDevice {
public:
    ~CoreAudioDevice() override { close(); }

    // ── Device lookup ────────────────────────────────────────────────────

    static AudioDeviceID find_device(const char* device) {
        if (!device || !device[0]) return 0;

        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
        if (size == 0) return 0;

        int count = (int)(size / sizeof(AudioDeviceID));
        AudioDeviceID* devs = new AudioDeviceID[count];
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devs);

        char* endp = nullptr;
        long idx = strtol(device, &endp, 10);
        bool is_idx = (endp && *endp == '\0' && idx >= 0);

        AudioDeviceID found = 0;
        int vis = 0;
        for (int i = 0; i < count; i++) {
            bool has = false;
            for (int s = 0; s < 2; s++) {
                AudioObjectPropertyAddress cp = {
                    kAudioDevicePropertyStreamConfiguration,
                    s == 0 ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                    kAudioObjectPropertyElementMain
                };
                UInt32 sz = 0;
                if (AudioObjectGetPropertyDataSize(devs[i], &cp, 0, nullptr, &sz) == noErr && sz > 0) {
                    AudioBufferList* b = (AudioBufferList*)malloc(sz);
                    if (AudioObjectGetPropertyData(devs[i], &cp, 0, nullptr, &sz, b) == noErr)
                        for (UInt32 j = 0; j < b->mNumberBuffers; j++)
                            if (b->mBuffers[j].mNumberChannels > 0) has = true;
                    free(b);
                }
            }
            if (!has) continue;
            if (is_idx && vis == (int)idx) { found = devs[i]; break; }
            if (!is_idx) {
                CFStringRef nr = nullptr;
                AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                UInt32 ns = sizeof(nr);
                AudioObjectGetPropertyData(devs[i], &np, 0, nullptr, &ns, &nr);
                if (nr) {
                    char n[256];
                    CFStringGetCString(nr, n, sizeof(n), kCFStringEncodingUTF8);
                    CFRelease(nr);
                    if (strstr(n, device)) { found = devs[i]; break; }
                }
            }
            vis++;
        }
        delete[] devs;
        return found;
    }

    static bool set_aq_device(AudioQueueRef q, AudioDeviceID d) {
        if (d == 0) return true;
        CFStringRef uid = nullptr;
        AudioObjectPropertyAddress p = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 s = sizeof(uid);
        if (AudioObjectGetPropertyData(d, &p, 0, nullptr, &s, &uid) != noErr || !uid) return false;
        OSStatus e = AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
        CFRelease(uid);
        return e == noErr;
    }

    static void print_device_name(AudioDeviceID d) {
        if (d == 0) return;
        CFStringRef nr = nullptr;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 ns = sizeof(nr);
        AudioObjectGetPropertyData(d, &np, 0, nullptr, &ns, &nr);
        if (nr) {
            char n[256];
            CFStringGetCString(nr, n, sizeof(n), kCFStringEncodingUTF8);
            CFRelease(nr);
            fprintf(stderr, "  Audio device: %s\n", n);
        }
    }

    // ── Open ─────────────────────────────────────────────────────────────

    bool open(const char* device, int sample_rate, bool capture, bool playback) override {
        sample_rate_ = sample_rate;
        dev_id_ = find_device(device);
        if (device && device[0] && dev_id_ == 0) {
            fprintf(stderr, "[Audio] Device not found: '%s'\n", device);
            return false;
        }
        print_device_name(dev_id_);

        // 16-bit signed mono at requested rate
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sample_rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBitsPerChannel = 16;
        fmt.mChannelsPerFrame = 1;
        fmt.mBytesPerFrame = 2;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerPacket = 2;

        // ── Capture: AudioQueue (converts USB 48000→44100 automatically) ──
        if (capture) {
            OSStatus e = AudioQueueNewInput(&fmt, rx_callback, this, nullptr,
                                            kCFRunLoopCommonModes, 0, &in_q_);
            if (e != noErr) { fprintf(stderr, "[Audio] Capture queue failed: %d\n", (int)e); return false; }
            set_aq_device(in_q_, dev_id_);
            for (int i = 0; i < AQ_BUFS; i++) {
                AudioQueueAllocateBuffer(in_q_, AQ_FRAMES * 2, &in_buf_[i]);
                AudioQueueEnqueueBuffer(in_q_, in_buf_[i], 0, nullptr);
            }
            AudioQueueStart(in_q_, nullptr);
            has_rx_ = true;
            fprintf(stderr, "  Capture: AudioQueue @ %d Hz 16-bit mono\n", sample_rate);
        }

        // ── Output: AudioUnit AUHAL (reliable on USB devices) ──
        if (playback) {
            AudioComponentDescription desc{};
            desc.componentType = kAudioUnitType_Output;
            desc.componentSubType = kAudioUnitSubType_HALOutput;
            desc.componentManufacturer = kAudioUnitManufacturer_Apple;
            AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
            if (!comp) { fprintf(stderr, "[Audio] AUHAL not found\n"); return false; }

            OSStatus e = AudioComponentInstanceNew(comp, &au_);
            if (e != noErr) { fprintf(stderr, "[Audio] AU create: %d\n", (int)e); return false; }

            // Output only — no input on this unit
            UInt32 v = 0;
            AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &v, sizeof(v));
            v = 1;
            AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &v, sizeof(v));

            if (dev_id_)
                AudioUnitSetProperty(au_, kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global, 0, &dev_id_, sizeof(dev_id_));

            // Match HW output rate (AudioUnit doesn't convert on USB)
            AudioStreamBasicDescription hw{};
            UInt32 hs = sizeof(hw);
            AudioUnitGetProperty(au_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hw, &hs);
            out_rate_ = (hw.mSampleRate > 0) ? (int)hw.mSampleRate : sample_rate;

            AudioStreamBasicDescription ofmt = fmt;
            ofmt.mSampleRate = out_rate_;
            AudioUnitSetProperty(au_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &ofmt, sizeof(ofmt));

            // Render callback
            AURenderCallbackStruct cb{};
            cb.inputProc = tx_callback;
            cb.inputProcRefCon = this;
            AudioUnitSetProperty(au_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));

            AudioUnitInitialize(au_);
            AudioOutputUnitStart(au_);
            has_tx_ = true;
            fprintf(stderr, "  Output: AudioUnit @ %d Hz 16-bit mono\n", out_rate_);
        }

        return true;
    }

    // ── RX ───────────────────────────────────────────────────────────────

    int read(int16_t* buf, int frames) override {
        if (!has_rx_) return 0;
        for (;;) {
            int rd = rx_rd_.load(std::memory_order_relaxed);
            int wr = rx_wr_.load(std::memory_order_acquire);
            int avail = (wr - rd + RX_RING) % RX_RING;
            if (avail > 0) {
                int n = (frames < avail) ? frames : avail;
                for (int i = 0; i < n; i++)
                    buf[i] = rx_ring_[(rd + i) % RX_RING];
                rx_rd_.store((rd + n) % RX_RING, std::memory_order_release);
                return n;
            }
            if (!has_rx_) return 0;
            usleep(500);
        }
    }

    // ── TX: accumulate at modem rate, resample to output rate in drain ───
    // TX ring is SPSC lock-free: wait_drain is the sole writer,
    // tx_callback is the sole reader (CoreAudio real-time thread).
    // No mutex in the render callback — avoids priority inversion / AU reset.

    int write(const int16_t* buf, int frames) override {
        if (!has_tx_) return 0;
        for (int i = 0; i < frames; i++)
            tx_pcm_.push_back(buf[i]);
        return frames;
    }

    void flush() override {}

    void wait_drain() override {
        if (tx_pcm_.empty() || !has_tx_) return;

        // Resample if modem rate ≠ output HW rate
        std::vector<int16_t>* src = &tx_pcm_;
        std::vector<int16_t> resampled;
        if (out_rate_ != sample_rate_ && sample_rate_ > 0) {
            double ratio = (double)out_rate_ / sample_rate_;
            int out_len = (int)(tx_pcm_.size() * ratio) + 1;
            resampled.resize(out_len);
            for (int i = 0; i < out_len; i++) {
                double pos = i / ratio;
                int idx = (int)pos;
                if (idx >= (int)tx_pcm_.size() - 1) idx = (int)tx_pcm_.size() - 2;
                if (idx < 0) idx = 0;
                double frac = pos - idx;
                resampled[i] = (int16_t)(tx_pcm_[idx] * (1.0 - frac) + tx_pcm_[idx + 1] * frac);
            }
            src = &resampled;
        }

        // Write to TX ring (lock-free: single producer, no mutex)
        int wr = tx_wr_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < src->size(); i++) {
            int next = (wr + 1) % TX_RING;
            if (next == tx_rd_.load(std::memory_order_acquire)) break; // ring full — drop
            tx_ring_[wr] = (*src)[i];
            wr = next;
        }
        tx_wr_.store(wr, std::memory_order_release);

        // Wait for ring to drain: 1ms poll, hard timeout = expected duration + 200ms
        int drain_ms = (int)(1000L * (long)src->size() / out_rate_) + 200;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(drain_ms);
        while (true) {
            int avail = (tx_wr_.load(std::memory_order_acquire) -
                         tx_rd_.load(std::memory_order_acquire) + TX_RING) % TX_RING;
            if (avail == 0) break;
            if (std::chrono::steady_clock::now() >= deadline) break;
            usleep(1000);
        }

        tx_pcm_.clear();
    }

    // ── Close ────────────────────────────────────────────────────────────

    void close() override {
        if (in_q_) {
            AudioQueueStop(in_q_, true);
            AudioQueueDispose(in_q_, true);
            in_q_ = nullptr;
        }
        has_rx_ = false;

        if (au_) {
            AudioOutputUnitStop(au_);
            AudioUnitUninitialize(au_);
            AudioComponentInstanceDispose(au_);
            au_ = nullptr;
        }
        has_tx_ = false;
    }

private:
    AudioDeviceID dev_id_ = 0;

    // ── Capture (AudioQueue) ─────────────────────────────────────────────
    AudioQueueRef       in_q_ = nullptr;
    AudioQueueBufferRef in_buf_[AQ_BUFS]{};
    bool has_rx_ = false;

    int16_t rx_ring_[RX_RING]{};
    std::atomic<int> rx_wr_{0};
    std::atomic<int> rx_rd_{0};

    // ── Output (AudioUnit) ───────────────────────────────────────────────
    AudioUnit au_ = nullptr;
    bool has_tx_ = false;
    int  out_rate_ = 44100;

    // TX: modem accumulates here; wait_drain resamples + copies to tx_ring_
    std::vector<int16_t> tx_pcm_;

    // TX ring — SPSC lock-free (wait_drain writes, tx_callback reads)
    int16_t              tx_ring_[TX_RING]{};
    std::atomic<int>     tx_wr_{0};
    std::atomic<int>     tx_rd_{0};

    // ── Callbacks ────────────────────────────────────────────────────────

    // AudioQueue capture callback (lock-free write to RX ring)
    static void rx_callback(void* ctx, AudioQueueRef, AudioQueueBufferRef buf,
                            const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription*) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);
        int frames = (int)(buf->mAudioDataByteSize / 2);
        auto* samples = static_cast<int16_t*>(buf->mAudioData);

        int wr = self->rx_wr_.load(std::memory_order_relaxed);
        int rd = self->rx_rd_.load(std::memory_order_acquire);
        for (int i = 0; i < frames; i++) {
            int next = (wr + 1) % RX_RING;
            if (next == rd) break;  // full
            self->rx_ring_[wr] = samples[i];
            wr = next;
        }
        self->rx_wr_.store(wr, std::memory_order_release);

        AudioQueueEnqueueBuffer(self->in_q_, buf, 0, nullptr);
    }

    // AudioUnit output render callback — lock-free, real-time safe
    static OSStatus tx_callback(void* ctx, AudioUnitRenderActionFlags*,
                                const AudioTimeStamp*, UInt32, UInt32 frames,
                                AudioBufferList* ioData) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);
        int16_t* out = static_cast<int16_t*>(ioData->mBuffers[0].mData);
        int n = (int)frames;

        int rd = self->tx_rd_.load(std::memory_order_relaxed);
        int wr = self->tx_wr_.load(std::memory_order_acquire);
        int avail = (wr - rd + TX_RING) % TX_RING;
        int to_read = (n < avail) ? n : avail;

        for (int i = 0; i < to_read; i++) {
            out[i] = self->tx_ring_[rd];
            rd = (rd + 1) % TX_RING;
        }
        for (int i = to_read; i < n; i++)
            out[i] = 0;

        self->tx_rd_.store(rd, std::memory_order_release);
        return noErr;
    }
};

AudioDevice* AudioDevice::create() {
    return new CoreAudioDevice();
}

// ── list_devices ─────────────────────────────────────────────────────────

int AudioDevice::list_devices() {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
    if (size == 0) { printf("No audio devices found.\n"); return 0; }

    int count = (int)(size / sizeof(AudioDeviceID));
    auto* devs = new AudioDeviceID[count];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devs);

    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "#", "Device Name", "IN", "OUT", "UID");
    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "---", "----------------------------------------", "-----", "-----", "---");

    int listed = 0;
    for (int i = 0; i < count; i++) {
        CFStringRef nr = nullptr;
        AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 ns = sizeof(nr);
        AudioObjectGetPropertyData(devs[i], &np, 0, nullptr, &ns, &nr);
        char name[256] = "(unknown)";
        if (nr) { CFStringGetCString(nr, name, sizeof(name), kCFStringEncodingUTF8); CFRelease(nr); }

        CFStringRef ur = nullptr;
        AudioObjectPropertyAddress up = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 us = sizeof(ur);
        AudioObjectGetPropertyData(devs[i], &up, 0, nullptr, &us, &ur);
        char uid[256] = "";
        if (ur) { CFStringGetCString(ur, uid, sizeof(uid), kCFStringEncodingUTF8); CFRelease(ur); }

        bool has_in = false, has_out = false;
        for (int s = 0; s < 2; s++) {
            AudioObjectPropertyAddress cp = {
                kAudioDevicePropertyStreamConfiguration,
                s == 0 ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize(devs[i], &cp, 0, nullptr, &sz) == noErr && sz > 0) {
                AudioBufferList* b = (AudioBufferList*)malloc(sz);
                if (AudioObjectGetPropertyData(devs[i], &cp, 0, nullptr, &sz, b) == noErr)
                    for (UInt32 j = 0; j < b->mNumberBuffers; j++)
                        if (b->mBuffers[j].mNumberChannels > 0)
                            (s == 0 ? has_in : has_out) = true;
                free(b);
            }
        }
        if (!has_in && !has_out) continue;
        printf("%-4d  %-40s  %-5s  %-5s  %s\n", listed, name,
               has_in ? "yes" : "-", has_out ? "yes" : "-", uid);
        listed++;
    }
    delete[] devs;
    printf("\nFound %d audio device(s).\n", listed);
    return listed;
}

#endif // __APPLE__
