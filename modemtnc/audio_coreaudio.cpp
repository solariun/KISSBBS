// CoreAudio backend for macOS — system framework, no external deps
// Uses AudioQueue for capture and playback (simplest CoreAudio API)
#ifdef __APPLE__
#include "audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <condition_variable>

static constexpr int NUM_BUFFERS = 3;
static constexpr int BUFFER_FRAMES = 1024;

class CoreAudioDevice : public AudioDevice {
public:
    ~CoreAudioDevice() override { close(); }

    // Find AudioDeviceID by name or index (e.g. "1", "USB Audio Device")
    // Returns 0 (kAudioDeviceUnknown) if not found or empty string (use default)
    AudioDeviceID find_device(const char* device) {
        if (!device || !device[0]) return 0;

        // Get all devices
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

        // Check if device is a number (index from --list-devices)
        char* endp = nullptr;
        long idx = strtol(device, &endp, 10);
        bool is_index = (endp && *endp == '\0' && idx >= 0);

        AudioDeviceID found = 0;
        int visible = 0;
        for (int i = 0; i < count; i++) {
            // Check if device has any channels
            AudioObjectPropertyAddress inProp = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioDevicePropertyScopeInput,
                kAudioObjectPropertyElementMain
            };
            AudioObjectPropertyAddress outProp = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 inSz = 0, outSz = 0;
            bool has_in = false, has_out = false;
            if (AudioObjectGetPropertyDataSize(devices[i], &inProp, 0, nullptr, &inSz) == noErr && inSz > 0) {
                AudioBufferList* b = (AudioBufferList*)malloc(inSz);
                if (AudioObjectGetPropertyData(devices[i], &inProp, 0, nullptr, &inSz, b) == noErr)
                    for (UInt32 j = 0; j < b->mNumberBuffers; j++) if (b->mBuffers[j].mNumberChannels > 0) has_in = true;
                free(b);
            }
            if (AudioObjectGetPropertyDataSize(devices[i], &outProp, 0, nullptr, &outSz) == noErr && outSz > 0) {
                AudioBufferList* b = (AudioBufferList*)malloc(outSz);
                if (AudioObjectGetPropertyData(devices[i], &outProp, 0, nullptr, &outSz, b) == noErr)
                    for (UInt32 j = 0; j < b->mNumberBuffers; j++) if (b->mBuffers[j].mNumberChannels > 0) has_out = true;
                free(b);
            }
            if (!has_in && !has_out) continue;

            if (is_index && visible == (int)idx) {
                found = devices[i];
                break;
            }

            // Match by name (substring)
            if (!is_index) {
                CFStringRef nameRef = nullptr;
                AudioObjectPropertyAddress nameProp = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                UInt32 ns = sizeof(nameRef);
                AudioObjectGetPropertyData(devices[i], &nameProp, 0, nullptr, &ns, &nameRef);
                if (nameRef) {
                    char name[256];
                    CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
                    CFRelease(nameRef);
                    if (strstr(name, device) != nullptr) { found = devices[i]; break; }
                }
            }
            visible++;
        }
        delete[] devices;
        return found;
    }

    // Set the device on an AudioQueue by AudioDeviceID
    bool set_queue_device(AudioQueueRef queue, AudioDeviceID devId) {
        if (devId == 0) return true;  // use default

        // Get UID from device ID
        CFStringRef uidRef = nullptr;
        AudioObjectPropertyAddress uidProp = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 uidSz = sizeof(uidRef);
        if (AudioObjectGetPropertyData(devId, &uidProp, 0, nullptr, &uidSz, &uidRef) != noErr || !uidRef)
            return false;

        OSStatus err = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &uidRef, sizeof(uidRef));
        CFRelease(uidRef);
        if (err != noErr) {
            fprintf(stderr, "[Audio] Failed to set device on queue: %d\n", (int)err);
            return false;
        }
        return true;
    }

    bool open(const char* device, int sample_rate, bool capture, bool playback) override {
        sample_rate_ = sample_rate;

        AudioDeviceID devId = find_device(device);
        if (device && device[0] && devId == 0) {
            fprintf(stderr, "[Audio] Device not found: '%s'\n", device);
            fprintf(stderr, "        Run --list-devices to see available devices\n");
            return false;
        }
        if (devId != 0) {
            // Print selected device name
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

        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sample_rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBitsPerChannel = 16;
        fmt.mChannelsPerFrame = 1;  // mono
        fmt.mBytesPerFrame = 2;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerPacket = 2;

        if (capture) {
            OSStatus err = AudioQueueNewInput(&fmt, input_callback, this, nullptr,
                                              kCFRunLoopCommonModes, 0, &input_queue_);
            if (err != noErr) {
                fprintf(stderr, "[Audio] Failed to create input queue: %d\n", (int)err);
                return false;
            }
            set_queue_device(input_queue_, devId);
            for (int i = 0; i < NUM_BUFFERS; i++) {
                AudioQueueAllocateBuffer(input_queue_, BUFFER_FRAMES * 2, &input_bufs_[i]);
                AudioQueueEnqueueBuffer(input_queue_, input_bufs_[i], 0, nullptr);
            }
            err = AudioQueueStart(input_queue_, nullptr);
            if (err != noErr) {
                fprintf(stderr, "[Audio] Failed to start input: %d\n", (int)err);
                AudioQueueDispose(input_queue_, true);
                input_queue_ = nullptr;
                return false;
            }
            has_capture_ = true;
        }

        if (playback) {
            OSStatus err = AudioQueueNewOutput(&fmt, output_callback, this, nullptr,
                                               kCFRunLoopCommonModes, 0, &output_queue_);
            if (err != noErr) {
                fprintf(stderr, "[Audio] Failed to create output queue: %d\n", (int)err);
                return false;
            }
            set_queue_device(output_queue_, devId);
            for (int i = 0; i < NUM_BUFFERS; i++) {
                AudioQueueAllocateBuffer(output_queue_, BUFFER_FRAMES * 2, &output_bufs_[i]);
                output_bufs_[i]->mAudioDataByteSize = 0;
            }
            // Pre-fill with silence to start playback
            for (int i = 0; i < NUM_BUFFERS; i++) {
                memset(output_bufs_[i]->mAudioData, 0, BUFFER_FRAMES * 2);
                output_bufs_[i]->mAudioDataByteSize = BUFFER_FRAMES * 2;
                AudioQueueEnqueueBuffer(output_queue_, output_bufs_[i], 0, nullptr);
            }
            err = AudioQueueStart(output_queue_, nullptr);
            if (err != noErr) {
                fprintf(stderr, "[Audio] Failed to start output: %d\n", (int)err);
                AudioQueueDispose(output_queue_, true);
                output_queue_ = nullptr;
                return false;
            }
            has_playback_ = true;
        }

        return true;
    }

    int read(int16_t* buf, int frames) override {
        if (!has_capture_) return 0;
        std::unique_lock<std::mutex> lk(rx_mtx_);
        // Wait for data
        rx_cv_.wait(lk, [this] { return rx_avail_ > 0 || !has_capture_; });
        int n = std::min(frames, rx_avail_);
        memcpy(buf, rx_ring_ + rx_rd_, n * sizeof(int16_t));
        rx_rd_ = (rx_rd_ + n) % RX_RING_SIZE;
        rx_avail_ -= n;
        return n;
    }

    int write(const int16_t* buf, int frames) override {
        if (!has_playback_) return 0;
        std::unique_lock<std::mutex> lk(tx_mtx_);
        // Wait for free buffer
        tx_cv_.wait(lk, [this] { return tx_free_count_ > 0 || !has_playback_; });
        if (!has_playback_) return 0;

        AudioQueueBufferRef abuf = tx_free_bufs_[--tx_free_count_];
        int n = std::min(frames, BUFFER_FRAMES);
        memcpy(abuf->mAudioData, buf, n * sizeof(int16_t));
        abuf->mAudioDataByteSize = n * 2;
        AudioQueueEnqueueBuffer(output_queue_, abuf, 0, nullptr);
        return n;
    }

    void flush() override {
        if (output_queue_)
            AudioQueueFlush(output_queue_);
    }

    void close() override {
        if (input_queue_) {
            AudioQueueStop(input_queue_, true);
            AudioQueueDispose(input_queue_, true);
            input_queue_ = nullptr;
        }
        has_capture_ = false;
        rx_cv_.notify_all();

        if (output_queue_) {
            AudioQueueStop(output_queue_, true);
            AudioQueueDispose(output_queue_, true);
            output_queue_ = nullptr;
        }
        has_playback_ = false;
        tx_cv_.notify_all();
    }

private:
    // Capture
    AudioQueueRef          input_queue_ = nullptr;
    AudioQueueBufferRef    input_bufs_[NUM_BUFFERS]{};
    bool                   has_capture_ = false;

    static constexpr int RX_RING_SIZE = 16384;
    int16_t rx_ring_[RX_RING_SIZE]{};
    int     rx_wr_ = 0, rx_rd_ = 0, rx_avail_ = 0;
    std::mutex              rx_mtx_;
    std::condition_variable rx_cv_;

    // Playback
    AudioQueueRef          output_queue_ = nullptr;
    AudioQueueBufferRef    output_bufs_[NUM_BUFFERS]{};
    bool                   has_playback_ = false;

    AudioQueueBufferRef    tx_free_bufs_[NUM_BUFFERS]{};
    int                    tx_free_count_ = 0;
    std::mutex              tx_mtx_;
    std::condition_variable tx_cv_;

    static void input_callback(void* ctx, AudioQueueRef /*q*/, AudioQueueBufferRef buf,
                               const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription*) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);
        int frames = (int)(buf->mAudioDataByteSize / 2);
        auto* samples = static_cast<int16_t*>(buf->mAudioData);

        {
            std::lock_guard<std::mutex> lk(self->rx_mtx_);
            for (int i = 0; i < frames && self->rx_avail_ < RX_RING_SIZE; i++) {
                self->rx_ring_[self->rx_wr_] = samples[i];
                self->rx_wr_ = (self->rx_wr_ + 1) % RX_RING_SIZE;
                self->rx_avail_++;
            }
        }
        self->rx_cv_.notify_one();

        // Re-enqueue buffer
        AudioQueueEnqueueBuffer(self->input_queue_, buf, 0, nullptr);
    }

    static void output_callback(void* ctx, AudioQueueRef /*q*/, AudioQueueBufferRef buf) {
        auto* self = static_cast<CoreAudioDevice*>(ctx);
        std::lock_guard<std::mutex> lk(self->tx_mtx_);
        self->tx_free_bufs_[self->tx_free_count_++] = buf;
        self->tx_cv_.notify_one();
    }
};

AudioDevice* AudioDevice::create() {
    return new CoreAudioDevice();
}

int AudioDevice::list_devices() {
    // Get all audio devices via CoreAudio HAL
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size);
    if (err != noErr || size == 0) {
        printf("No audio devices found.\n");
        return 0;
    }

    int count = (int)(size / sizeof(AudioDeviceID));
    auto* devices = new AudioDeviceID[count];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, devices);

    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "#", "Device Name", "IN", "OUT", "UID");
    printf("%-4s  %-40s  %-5s  %-5s  %s\n", "---", "----------------------------------------", "-----", "-----", "---");

    int listed = 0;
    for (int i = 0; i < count; i++) {
        // Get device name
        CFStringRef nameRef = nullptr;
        AudioObjectPropertyAddress nameProp = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 nameSize = sizeof(nameRef);
        AudioObjectGetPropertyData(devices[i], &nameProp, 0, nullptr, &nameSize, &nameRef);

        char name[256] = "(unknown)";
        if (nameRef) {
            CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(nameRef);
        }

        // Get UID
        CFStringRef uidRef = nullptr;
        AudioObjectPropertyAddress uidProp = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 uidSize = sizeof(uidRef);
        AudioObjectGetPropertyData(devices[i], &uidProp, 0, nullptr, &uidSize, &uidRef);

        char uid[256] = "";
        if (uidRef) {
            CFStringGetCString(uidRef, uid, sizeof(uid), kCFStringEncodingUTF8);
            CFRelease(uidRef);
        }

        // Check input channels
        AudioObjectPropertyAddress inProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 inSize = 0;
        bool has_input = false;
        if (AudioObjectGetPropertyDataSize(devices[i], &inProp, 0, nullptr, &inSize) == noErr && inSize > 0) {
            auto* bufs = (AudioBufferList*)malloc(inSize);
            if (AudioObjectGetPropertyData(devices[i], &inProp, 0, nullptr, &inSize, bufs) == noErr) {
                for (UInt32 b = 0; b < bufs->mNumberBuffers; b++)
                    if (bufs->mBuffers[b].mNumberChannels > 0) has_input = true;
            }
            free(bufs);
        }

        // Check output channels
        AudioObjectPropertyAddress outProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 outSize = 0;
        bool has_output = false;
        if (AudioObjectGetPropertyDataSize(devices[i], &outProp, 0, nullptr, &outSize) == noErr && outSize > 0) {
            auto* bufs = (AudioBufferList*)malloc(outSize);
            if (AudioObjectGetPropertyData(devices[i], &outProp, 0, nullptr, &outSize, bufs) == noErr) {
                for (UInt32 b = 0; b < bufs->mNumberBuffers; b++)
                    if (bufs->mBuffers[b].mNumberChannels > 0) has_output = true;
            }
            free(bufs);
        }

        if (!has_input && !has_output) continue;

        printf("%-4d  %-40s  %-5s  %-5s  %s\n",
               listed, name,
               has_input  ? "yes" : "-",
               has_output ? "yes" : "-",
               uid);
        listed++;
    }

    delete[] devices;
    printf("\nFound %d audio device(s).\n", listed);
    printf("\nUsage:  modemtnc -d \"<Device Name>\" ...\n");
    printf("  (macOS currently uses system default — device selection by name coming soon)\n");
    return listed;
}

#endif // __APPLE__
