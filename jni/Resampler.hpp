#pragma once

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#include <vector>
#include <cstdint>

class Resampler {
public:
    Resampler();
    ~Resampler();

    bool init(int in_rate, AVChannelLayout in_layout, AVSampleFormat in_fmt,
              int out_rate, AVChannelLayout out_layout, AVSampleFormat out_fmt);

    // Returns number of output samples per channel
    int convert(const uint8_t** in_data, int in_samples, uint8_t** out_data, int max_out_samples);

    int getDelay(int in_rate);

private:
    SwrContext* m_swr = nullptr;
    AVSampleFormat m_out_fmt;
    int m_out_channels;
};
