// CoreAudio backend for macOS using AudioUnit (AUHAL)
//
// Uses the Audio Unit Hardware Abstraction Layer for reliable full-duplex
// audio on USB devices (Digirig, etc.). AudioQueue had issues where only
// the first output burst would play on USB audio devices.
//
// Architecture (same as PortAudio/Direwolf):
//   - One AUHAL AudioUnit for both input and output
//   - Input render callback → RX ring buffer
//   - Output render callback → reads from TX ring buffer (silence when empty)
//   - write() fills the TX ring buffer
//   - wait_drain() sleeps for calculated audio duration
//
// License: MIT (PortAudio-inspired, original code)
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
#include <mutex>
#include <condition_variable>
#include <unistd.h>

static constexpr int RX_RING_SIZE = 65536;  // ~1.5s at 44100 Hz
static constexpr int TX_RING_SIZE = 131072; // ~3s at 44100 Hz — holds a full TX burst

class CoreAudioDevice : public AudioDevice {
public:
    ~CoreAudioDevice() override { close(); }

    // ── Device lookup ────────────────────────────────────────────────────

    AudioDeviceID find_device(const char* device) {
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
        AudioDeviceID* devices = new AudioDeviceID[count];
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devices);

        char* endp = nullptr;
        long idx = strtol(device, &endp, 10);
        bool is_index = (endp && *endp == '\0' && idx >= 0);

        AudioDeviceID found = 0;
        int visible = 0;
        for (int i = 0; i < count; i++) {
            bool has_ch = false;
            for (int scope = 0; scope < 2; scope++) {
                AudioObjectPropertyAddress cp = {
                    kAudioDevicePropertyStreamConfiguration,
                    scope == 0 ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                    kAudioObjectPropertyElementMain
                };
                UInt32 sz = 0;
                if (AudioObjectGetPropertyDataSize(devices[i], &cp, 0, nullptr, &sz) == noErr && sz > 0) {
                    AudioBufferList* b = (AudioBufferList*)malloc(sz);
                    if (AudioObjectGetPropertyData(devices[i], &cp, 0, nullptr, &sz, b) == noErr)
                        for (UInt32 j = 0; j < b->mNumberBuffers; j++)
                            if (b->mBuffers[j].mNumberChannels > 0) has_ch = true;
                    free(b);
                }
            }
            if (!has_ch) continue;

            if (is_index && visible == (int)idx) { found = devices[i]; break; }
            if (!is_index) {
                CFStringRef nameRef = nullptr;
                AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                UInt32 ns = sizeof(nameRef);
                AudioObjectGetPropertyData(devices[i], &np, 0, nullptr, &ns, &nameRef);
                if (nameRef) {
                    char name[256];
                    CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
                    CFRelease(nameRef);
                    if (strstr(name, device)) { found = devices[i]; break; }
                }
            }
            visible++;
        }
        delete[] devices;
        return found;
    }

    // ── Open ─────────────────────────────────────────────────────────────

    bool open(const char* device, int sample_rate, bool capture, bool playback) override {
        sample_rate_ = sample_rate;
        has_capture_ = capture;
        has_playback_ = playback;

        AudioDeviceID devId = find_device(device);
        if (device && device[0] && devId == 0) {
            fprintf(stderr, "[Audio] Device not found: '%s'\n", device);
            return false;
        }

        if (devId != 0) {
            CFStringRef nameRef = nullptr;
            AudioObjectPropertyAddress np = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            UInt32 ns = sizeof(nameRef);
            AudioObjectGetPropertyData(devId, &np, 0, nullptr, &ns, &nameRef);
            if (nameRef) {
                char name[256];
                CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
                CFRelease(nameRef);
                fprintf(stderr, "  Audio device: %s\n", name);
            }
        }

        // Audio format: 16-bit signed integer, mono
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sample_rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBitsPerChannel = 16;
        fmt.mChannelsPerFrame = 1;
        fmt.mBytesPerFrame = 2;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerPacket = 2;

        // ── Create AUHAL AudioUnit ──
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) { fprintf(stderr, "[Audio] AUHAL component not found\n"); return false; }

        OSStatus err = AudioComponentInstanceNew(comp, &au_);
        if (err != noErr) { fprintf(stderr, "[Audio] AudioUnit create failed: %d\n", (int)err); return false; }

        // Enable input (capture) — must be done BEFORE setting device
        UInt32 flag = 1;
        if (capture) {
            err = AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO,
                                       kAudioUnitScope_Input, 1, &flag, sizeof(flag));
            if (err != noErr) fprintf(stderr, "[AU] EnableIO input: %d\n", (int)err);
        }

        // Enable output (playback)
        if (playback) {
            flag = 1;
            err = AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO,
                                       kAudioUnitScope_Output, 0, &flag, sizeof(flag));
            if (err != noErr) fprintf(stderr, "[AU] EnableIO output: %d\n", (int)err);
        } else {
            // If no playback, disable output (AUHAL has output enabled by default)
            flag = 0;
            AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO,
                                 kAudioUnitScope_Output, 0, &flag, sizeof(flag));
        }

        // Set device
        if (devId != 0) {
            err = AudioUnitSetProperty(au_, kAudioOutputUnitProperty_CurrentDevice,
                                       kAudioUnitScope_Global, 0, &devId, sizeof(devId));
            if (err != noErr) fprintf(stderr, "[AU] SetDevice: %d\n", (int)err);
        }

        // Set format on input scope (what we receive from hardware)
        if (capture) {
            // First check what format the hardware provides
            AudioStreamBasicDescription hwFmt{};
            UInt32 fmtSz = sizeof(hwFmt);
            AudioUnitGetProperty(au_, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input, 1, &hwFmt, &fmtSz);
            fprintf(stderr, "  [AU] HW input: rate=%.0f ch=%u bits=%u\n",
                    hwFmt.mSampleRate, (unsigned)hwFmt.mChannelsPerFrame,
                    (unsigned)hwFmt.mBitsPerChannel);

            // Use hardware native sample rate — AudioUnit doesn't reliably
            // convert sample rates on USB devices
            if (hwFmt.mSampleRate > 0) {
                sample_rate_ = (int)hwFmt.mSampleRate;
                fmt.mSampleRate = hwFmt.mSampleRate;
            }
            fprintf(stderr, "  [AU] Using %d Hz 16-bit mono\n", sample_rate_);

            err = AudioUnitSetProperty(au_, kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
            if (err != noErr) fprintf(stderr, "[AU] SetFormat input: %d\n", (int)err);

            // Set input callback
            AURenderCallbackStruct inputCb{};
            inputCb.inputProc = input_render_cb;
            inputCb.inputProcRefCon = this;
            err = AudioUnitSetProperty(au_, kAudioOutputUnitProperty_SetInputCallback,
                                       kAudioUnitScope_Global, 0, &inputCb, sizeof(inputCb));
            if (err != noErr) fprintf(stderr, "[AU] SetInputCallback: %d\n", (int)err);
        }

        // Set format on output scope (what we send to hardware)
        if (playback) {
            fmt.mSampleRate = sample_rate_;  // same as what modem generates
            err = AudioUnitSetProperty(au_, kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
            if (err != noErr) fprintf(stderr, "[AU] SetFormat output: %d\n", (int)err);

            // Set output render callback
            AURenderCallbackStruct outputCb{};
            outputCb.inputProc = output_render_cb;
            outputCb.inputProcRefCon = this;
            err = AudioUnitSetProperty(au_, kAudioUnitProperty_SetRenderCallback,
                                       kAudioUnitScope_Input, 0, &outputCb, sizeof(outputCb));
            if (err != noErr) fprintf(stderr, "[AU] SetRenderCallback: %d\n", (int)err);
        }

        // Initialize and start
        err = AudioUnitInitialize(au_);
        if (err != noErr) {
            fprintf(stderr, "[Audio] AudioUnit init failed: %d\n", (int)err);
            AudioComponentInstanceDispose(au_); au_ = nullptr;
            return false;
        }

        err = AudioOutputUnitStart(au_);
        if (err != noErr) {
            fprintf(stderr, "[Audio] AudioUnit start failed: %d\n", (int)err);
            AudioUnitUninitialize(au_);
            AudioComponentInstanceDispose(au_); au_ = nullptr;
            return false;
        }

        return true;
    }

    // ── RX: read from ring buffer ────────────────────────────────────────

    int read(int16_t* buf, int frames) override {
        if (!has_capture_) return 0;
        // Spin until data available (lock-free)
        int rd, wr, avail;
        for (;;) {
            rd = rx_rd_.load(std::memory_order_relaxed);
            wr = rx_wr_.load(std::memory_order_acquire);
            avail = (wr - rd + RX_RING_SIZE) % RX_RING_SIZE;
            if (avail > 0 || !has_capture_) break;
            usleep(500);  // 0.5ms — minimal sleep, don't starve CPU
        }
        if (!has_capture_) return 0;
        int n = std::min(frames, avail);
        for (int i = 0; i < n; i++) {
            buf[i] = rx_ring_[(rd + i) % RX_RING_SIZE];
        }
        rx_rd_.store((rd + n) % RX_RING_SIZE, std::memory_order_release);
        return n;
    }

    // ── TX: write to ring buffer ─────────────────────────────────────────

    int write(const int16_t* buf, int frames) override {
        if (!has_playback_) return 0;
        std::lock_guard<std::mutex> lk(tx_mtx_);
        int n = frames;
        for (int i = 0; i < n; i++) {
            tx_ring_[tx_wr_] = buf[i];
            tx_wr_ = (tx_wr_ + 1) % TX_RING_SIZE;
            if (tx_avail_ < TX_RING_SIZE) tx_avail_++;
        }
        return n;
    }

    void flush() override {}

    // ── TX: wait for audio to play ───────────────────────────────────────
    // Like Direwolf: sleep for calculated audio duration.
    // The output render callback reads from tx_ring_ at audio sample rate.

    void wait_drain() override {
        if (!has_playback_) return;
        int avail;
        {
            std::lock_guard<std::mutex> lk(tx_mtx_);
            avail = tx_avail_;
        }
        if (avail <= 0) return;
        int duration_ms = (int)(1000L * (long)avail / sample_rate_);
        usleep((unsigned)(duration_ms + 50) * 1000);  // audio duration + 50ms margin
    }

    // ── Close ────────────────────────────────────────────────────────────

    void close() override {
        if (au_) {
            AudioOutputUnitStop(au_);
            AudioUnitUninitialize(au_);
            AudioComponentInstanceDispose(au_);
            au_ = nullptr;
        }
        has_capture_ = false;
        has_playback_ = false;
    }

private:
    AudioUnit au_ = nullptr;
    bool has_capture_ = false;
    bool has_playback_ = false;

    // ── RX ring buffer (lock-free: audio callback writes, read() reads) ──
    int16_t rx_ring_[RX_RING_SIZE]{};
    std::atomic<int> rx_wr_{0};
    std::atomic<int> rx_rd_{0};

    // ── TX ring buffer ───────────────────────────────────────────────────
    int16_t tx_ring_[TX_RING_SIZE]{};
    int     tx_wr_ = 0, tx_rd_ = 0;
    std::atomic<int> tx_avail_{0};
    std::mutex tx_mtx_;

    // ── Input render callback (runs on audio thread) ─────────────────────
    static OSStatus input_render_cb(void* ctx,
                                    AudioUnitRenderActionFlags* ioActionFlags,
                                    const AudioTimeStamp* inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList* /*ioData*/) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);

        // Allocate buffer to receive input data
        int16_t tmp[4096];
        AudioBufferList bufList;
        bufList.mNumberBuffers = 1;
        bufList.mBuffers[0].mNumberChannels = 1;
        bufList.mBuffers[0].mDataByteSize = inNumberFrames * 2;
        bufList.mBuffers[0].mData = tmp;

        OSStatus err = AudioUnitRender(self->au_, ioActionFlags, inTimeStamp,
                                       inBusNumber, inNumberFrames, &bufList);
        if (err != noErr) return err;

        // Log first callback and count
        static int cb_count = 0;
        cb_count++;
        if (cb_count <= 3 || (cb_count % 1000 == 0)) {
            fprintf(stderr, "  [AU] input cb #%d: %u frames, %u bytes, err=%d\n",
                    cb_count, (unsigned)inNumberFrames,
                    (unsigned)bufList.mBuffers[0].mDataByteSize, (int)err);
        }

        // Copy to RX ring buffer (lock-free)
        int wr = self->rx_wr_.load(std::memory_order_relaxed);
        int rd = self->rx_rd_.load(std::memory_order_acquire);
        for (UInt32 i = 0; i < inNumberFrames; i++) {
            int next = (wr + 1) % RX_RING_SIZE;
            if (next == rd) break;
            self->rx_ring_[wr] = tmp[i];
            wr = next;
        }
        self->rx_wr_.store(wr, std::memory_order_release);
        return noErr;
    }

    // ── Output render callback (runs on audio thread) ────────────────────
    // Fills output buffer from TX ring buffer. Outputs silence when empty.
    static OSStatus output_render_cb(void* ctx,
                                     AudioUnitRenderActionFlags* /*ioActionFlags*/,
                                     const AudioTimeStamp* /*inTimeStamp*/,
                                     UInt32 /*inBusNumber*/,
                                     UInt32 inNumberFrames,
                                     AudioBufferList* ioData) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);
        int16_t* out = static_cast<int16_t*>(ioData->mBuffers[0].mData);
        int frames = (int)inNumberFrames;

        // Read from TX ring buffer (lock-free read with atomic avail)
        int avail = self->tx_avail_.load();
        int to_read = std::min(frames, avail);

        for (int i = 0; i < to_read; i++) {
            out[i] = self->tx_ring_[self->tx_rd_];
            self->tx_rd_ = (self->tx_rd_ + 1) % TX_RING_SIZE;
        }
        self->tx_avail_ -= to_read;

        // Fill remaining with silence
        for (int i = to_read; i < frames; i++)
            out[i] = 0;

        return noErr;
    }
};

AudioDevice* AudioDevice::create() {
    return new CoreAudioDevice();
}

// ── list_devices ─────────────────────────────────────────────────────────

int AudioDevice::list_devices() {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
    if (err != noErr || size == 0) { printf("No audio devices found.\n"); return 0; }

    int count = (int)(size / sizeof(AudioDeviceID));
    auto* devices = new AudioDeviceID[count];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devices);

    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "#", "Device Name", "IN", "OUT", "UID");
    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "---", "----------------------------------------", "-----", "-----", "---");

    int listed = 0;
    for (int i = 0; i < count; i++) {
        CFStringRef nameRef = nullptr;
        AudioObjectPropertyAddress nameProp = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 nameSize = sizeof(nameRef);
        AudioObjectGetPropertyData(devices[i], &nameProp, 0, nullptr, &nameSize, &nameRef);
        char name[256] = "(unknown)";
        if (nameRef) { CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8); CFRelease(nameRef); }

        CFStringRef uidRef = nullptr;
        AudioObjectPropertyAddress uidProp = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 uidSize = sizeof(uidRef);
        AudioObjectGetPropertyData(devices[i], &uidProp, 0, nullptr, &uidSize, &uidRef);
        char uid[256] = "";
        if (uidRef) { CFStringGetCString(uidRef, uid, sizeof(uid), kCFStringEncodingUTF8); CFRelease(uidRef); }

        bool has_input = false, has_output = false;
        for (int scope = 0; scope < 2; scope++) {
            AudioObjectPropertyAddress cp = {
                kAudioDevicePropertyStreamConfiguration,
                scope == 0 ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize(devices[i], &cp, 0, nullptr, &sz) == noErr && sz > 0) {
                AudioBufferList* b = (AudioBufferList*)malloc(sz);
                if (AudioObjectGetPropertyData(devices[i], &cp, 0, nullptr, &sz, b) == noErr)
                    for (UInt32 j = 0; j < b->mNumberBuffers; j++)
                        if (b->mBuffers[j].mNumberChannels > 0)
                            (scope == 0 ? has_input : has_output) = true;
                free(b);
            }
        }
        if (!has_input && !has_output) continue;

        printf("%-4d  %-40s  %-5s  %-5s  %s\n", listed, name,
               has_input ? "yes" : "-", has_output ? "yes" : "-", uid);
        listed++;
    }
    delete[] devices;
    printf("\nFound %d audio device(s).\n", listed);
    printf("\nUsage:  modemtnc -d \"<Device Name>\" ...\n");
    return listed;
}

#endif // __APPLE__
