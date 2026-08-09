#include "SoftwareMixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#define SWMIXER_HAVE_NEON 1
// #pragma message("NEON mixer backend enabled")
#endif

namespace {

#ifdef SWMIXER_HAVE_NEON
void applyVolumeNeon(int16_t *buffer, int samples, float factor) {
    const float32x4_t vfactor = vdupq_n_f32(factor);
    int i = 0;
    for (; i + 8 <= samples; i += 8) {
        int16x8_t s16 = vld1q_s16(buffer + i);

        float32x4_t lo_f = vcvtq_f32_s32(vmovl_s16(vget_low_s16(s16)));
        float32x4_t hi_f = vcvtq_f32_s32(vmovl_s16(vget_high_s16(s16)));

        lo_f = vmulq_f32(lo_f, vfactor);
        hi_f = vmulq_f32(hi_f, vfactor);

        int16x4_t lo_n = vqmovn_s32(vcvtq_s32_f32(lo_f));
        int16x4_t hi_n = vqmovn_s32(vcvtq_s32_f32(hi_f));

        vst1q_s16(buffer + i, vcombine_s16(lo_n, hi_n));
    }

    for (; i < samples; ++i) {
        double s = static_cast<double>(buffer[i]) * factor;
        s = std::clamp(
            s, static_cast<double>(std::numeric_limits<int16_t>::min()),
            static_cast<double>(std::numeric_limits<int16_t>::max())
        );
        buffer[i] = static_cast<int16_t>(s);
    }
}

#if defined(__aarch64__)
void applyVolumeNeon(int32_t *buffer, int samples, float factor) {
    const float64x2_t vfactor = vdupq_n_f64(static_cast<double>(factor));
    const float64x2_t vmax = vdupq_n_f64(static_cast<double>(std::numeric_limits<int32_t>::max()));
    const float64x2_t vmin = vdupq_n_f64(static_cast<double>(std::numeric_limits<int32_t>::min()));

    int i = 0;
    for (; i + 2 <= samples; i += 2) {
        int32x2_t s32 = vld1_s32(buffer + i);
        float64x2_t f = vcvtq_f64_s64(vmovl_s32(s32)); // widen s32->s64->f64

        f = vmulq_f64(f, vfactor);
        f = vminq_f64(vmaxq_f64(f, vmin), vmax); // clamp before narrowing

        int32x2_t out = vqmovn_s64(vcvtq_s64_f64(f)); // truncate + saturate
        vst1_s32(buffer + i, out);
    }

    for (; i < samples; ++i) {
        double s = static_cast<double>(buffer[i]) * factor;
        s = std::clamp(
            s, static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())
        );
        buffer[i] = static_cast<int32_t>(s);
    }
}
#endif // __aarch64__
#endif // SWMIXER_HAVE_NEON

} // namespace

float SoftwareMixer::getVolumeFactor(float volume) {
    if (volume <= 0.0f) return 0.0f;
    if (volume >= 1.0f) return 1.0f;
    // 40dB range: factor = 10 ^ (db / 20).
    // At volume 0.0, dB is -40 (factor 0.01). At volume 1.0, dB is 0 (factor 1.0).
    return powf(10.0f, 2.0f * (volume - 1.0f));
}

template <typename T>
void SoftwareMixer::applyVolume(T *buffer, int samples, float factor) {
    if (std::abs(factor - 1.0f) < 0.001f) return;

    if constexpr (std::is_same_v<T, int16_t>) {
#ifdef SWMIXER_HAVE_NEON
        applyVolumeNeon(buffer, samples, factor);
        return;
#endif
    } else if constexpr (std::is_same_v<T, int32_t>) {
#if defined(SWMIXER_HAVE_NEON) && defined(__aarch64__)
        applyVolumeNeon(buffer, samples, factor);
        return;
#endif
    }

    for (int i = 0; i < samples; ++i) {
        double s = static_cast<double>(buffer[i]) * factor;

        if (s > static_cast<double>(std::numeric_limits<T>::max())) {
            s = static_cast<double>(std::numeric_limits<T>::max());
        } else if (s < static_cast<double>(std::numeric_limits<T>::min())) {
            s = static_cast<double>(std::numeric_limits<T>::min());
        }

        buffer[i] = static_cast<T>(s);
    }
}

template void SoftwareMixer::applyVolume<int16_t>(int16_t *, int, float);
template void SoftwareMixer::applyVolume<int32_t>(int32_t *, int, float);

void SoftwareMixer::applyVolumeFloat(float *buffer, int samples, float factor) {
    if (std::abs(factor - 1.0f) < 0.001f) return;

#ifdef SWMIXER_HAVE_NEON
    const float32x4_t vfactor = vdupq_n_f32(factor);
    int i = 0;
    for (; i + 4 <= samples; i += 4) {
        float32x4_t v = vld1q_f32(buffer + i);
        v = vmulq_f32(v, vfactor);
        vst1q_f32(buffer + i, v);
    }
    for (; i < samples; ++i) {
        buffer[i] *= factor;
    }
#else
    for (int i = 0; i < samples; ++i) {
        buffer[i] *= factor;
    }
#endif
}
