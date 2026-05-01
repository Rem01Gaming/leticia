#include "Replaygain.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

ReplayGainProcessor::ReplayGainProcessor() {
}

void ReplayGainProcessor::parseMetadata(AVDictionary *metadata) {
    if (!metadata) return;

    AVDictionaryEntry *gain_entry = av_dict_get(metadata, "REPLAYGAIN_TRACK_GAIN", nullptr, 0);
    AVDictionaryEntry *peak_entry = av_dict_get(metadata, "REPLAYGAIN_TRACK_PEAK", nullptr, 0);

    if (gain_entry) {
        m_track_gain = static_cast<float>(atof(gain_entry->value));
        m_available = true;
    }

    if (peak_entry) {
        m_track_peak = static_cast<float>(atof(peak_entry->value));
        m_available = true;
    }

    if (m_available) {
        // Multiplier = 10^(gain / 20)
        m_multiplier = powf(10.0f, m_track_gain / 20.0f);

        // Prevent clipping based on peak
        if (m_track_peak > 0.0f && m_multiplier * m_track_peak > 1.0f) {
            m_multiplier = 1.0f / m_track_peak;
        }
    }
}
