#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <string>

/**
 * @brief Handles the extraction and calculation of ReplayGain values from media metadata.
 */
class ReplayGainProcessor {
public:
    /**
     * @brief Default constructor. Initializes with a neutral multiplier of 1.0.
     */
    ReplayGainProcessor();

    /**
     * @brief Parses an FFmpeg metadata dictionary for ReplayGain tags.
     * @param metadata A pointer to an AVFormat AVDictionary containing tags.
     */
    void parseMetadata(AVDictionary *metadata);

    /**
     * @brief Gets the calculated volume multiplier.
     * @return A float representing the gain factor (e.g., 0.5 for -6dB).
     */
    float getMultiplier() const {
        return m_multiplier;
    }

    /**
     * @brief Checks if valid ReplayGain data was successfully parsed.
     * @return true if gain or peak data is available, false otherwise.
     */
    bool isAvailable() const {
        return m_available;
    }

private:
    /** @brief The track gain value in decibels (dB). */
    float m_track_gain = 0.0f;
    
    /** @brief The track peak signal level (1.0 = full scale). */
    float m_track_peak = 0.0f;
    
    /** @brief The final calculated multiplier applied to audio samples. */
    float m_multiplier = 1.0f;
    
    /** @brief Flag indicating if ReplayGain metadata was found. */
    bool m_available = false;
};