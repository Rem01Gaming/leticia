#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

/**
 * @brief A utility class providing static methods for audio volume attenuation and buffer scaling.
 */
class SoftwareMixer {
public:
    /**
     * @brief Calculates a logarithmic volume attenuation factor.
     * @param volume The input volume level, expected range [0.0, 1.0].
     * @return float The calculated multiplier factor. Returns 0.0 if volume <= 0 and 1.0 if volume >= 1.
     */
    static float getVolumeFactor(float volume) {
        if (volume <= 0.0f) return 0.0f;
        if (volume >= 1.0f) return 1.0f;
        // 40dB range: factor = 10 ^ (db / 20).
        // At volume 0.0, dB is -40 (factor 0.01). At volume 1.0, dB is 0 (factor 1.0).
        return powf(10.0f, 2.0f * (volume - 1.0f));
    }

    /**
     * @brief Applies a volume factor to a buffer of generic numeric samples.
     * @tparam T The numeric type of the audio samples (e.g., int16_t, int32_t).
     * @param buffer Pointer to the array of audio samples.
     * @param samples The number of samples to process.
     * @param factor The multiplication factor to apply to every sample.
     */
    template <typename T>
    static void applyVolume(T *buffer, int samples, float factor) {
        // Optimization: Skip processing if factor is effectively 1.0
        if (std::abs(factor - 1.0f) < 0.001f) return;

        for (int i = 0; i < samples; ++i) {
            double s = static_cast<double>(buffer[i]) * factor;

            // Perform saturation/clipping
            if (s > static_cast<double>(std::numeric_limits<T>::max())) {
                s = std::numeric_limits<T>::max();
            } else if (s < static_cast<double>(std::numeric_limits<T>::min())) {
                s = std::numeric_limits<T>::min();
            }

            buffer[i] = static_cast<T>(s);
        }
    }

    /**
     * @brief Applies a volume factor to a floating-point audio buffer.
     * @param buffer Pointer to the array of float samples.
     * @param samples The number of samples to process.
     * @param factor The multiplication factor to apply.
     */
    static void applyVolumeFloat(float *buffer, int samples, float factor) {
        if (std::abs(factor - 1.0f) < 0.001f) return;

        for (int i = 0; i < samples; ++i) {
            buffer[i] *= factor;
        }
    }
};
