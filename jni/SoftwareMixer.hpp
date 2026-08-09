#pragma once

#include <cstdint>

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
    static float getVolumeFactor(float volume);

    /**
     * @brief Applies a volume factor to a buffer of generic numeric samples.
     * @tparam T The numeric type of the audio samples (e.g., int16_t, int32_t).
     * @param buffer Pointer to the array of audio samples.
     * @param samples The number of samples to process.
     * @param factor The multiplication factor to apply to every sample.
     */
    template <typename T>
    static void applyVolume(T *buffer, int samples, float factor);

    /**
     * @brief Applies a volume factor to a floating-point audio buffer.
     * @param buffer Pointer to the array of float samples.
     * @param samples The number of samples to process.
     * @param factor The multiplication factor to apply.
     */
    static void applyVolumeFloat(float *buffer, int samples, float factor);
};

extern template void SoftwareMixer::applyVolume<int16_t>(int16_t *, int, float);
extern template void SoftwareMixer::applyVolume<int32_t>(int32_t *, int, float);
