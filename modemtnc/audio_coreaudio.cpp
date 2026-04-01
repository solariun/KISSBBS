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

    bool open(const char* /*device*/, int sample_rate, bool capture, bool playback) override {
        sample_rate_ = sample_rate;

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

#endif // __APPLE__
