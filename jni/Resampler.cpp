#include "Resampler.hpp"

extern "C" {
#include <libavutil/opt.h>
}

Resampler::Resampler() {}

Resampler::~Resampler() {
    if (m_swr) swr_free(&m_swr);
}

bool Resampler::init(int in_rate, AVChannelLayout in_layout, AVSampleFormat in_fmt,
                    int out_rate, AVChannelLayout out_layout, AVSampleFormat out_fmt) {
    if (m_swr) swr_free(&m_swr);

    m_swr = swr_alloc();
    av_opt_set_chlayout(m_swr, "in_chlayout", &in_layout, 0);
    av_opt_set_int(m_swr, "in_sample_rate", in_rate, 0);
    av_opt_set_sample_fmt(m_swr, "in_sample_fmt", in_fmt, 0);

    av_opt_set_chlayout(m_swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(m_swr, "out_sample_rate", out_rate, 0);
    av_opt_set_sample_fmt(m_swr, "out_sample_fmt", out_fmt, 0);

    m_out_fmt = out_fmt;
    m_out_channels = out_layout.nb_channels;

    return swr_init(m_swr) >= 0;
}

int Resampler::convert(const uint8_t** in_data, int in_samples, uint8_t** out_data, int max_out_samples) {
    if (!m_swr) return -1;
    return swr_convert(m_swr, out_data, max_out_samples, in_data, in_samples);
}

int Resampler::getDelay(int in_rate) {
    if (!m_swr) return 0;
    return swr_get_delay(m_swr, in_rate);
}
