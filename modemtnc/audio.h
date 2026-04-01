// Platform audio abstraction — ALSA (Linux) / CoreAudio (macOS)
// No external dependencies — system frameworks only
#pragma once
#include <cstdint>
#include <cstddef>

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    // Open audio device for capture and/or playback
    // Returns true on success
    virtual bool open(const char* device, int sample_rate, bool capture, bool playback) = 0;

    // Read samples from capture (blocking).  Returns frames actually read.
    virtual int read(int16_t* buf, int frames) = 0;

    // Write samples to playback (blocking).  Returns frames actually written.
    virtual int write(const int16_t* buf, int frames) = 0;

    // Flush any buffered output
    virtual void flush() = 0;

    // Close device
    virtual void close() = 0;

    // File descriptor for select()/poll() on capture side (-1 if not available)
    virtual int capture_fd() const { return -1; }

    int sample_rate() const { return sample_rate_; }

    // Factory: create platform-appropriate audio device
    static AudioDevice* create();

protected:
    int sample_rate_ = 44100;
};
