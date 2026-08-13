#include "DsdPacker.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#define DSD_PACKER_HAVE_NEON 1
#endif

namespace {

constexpr uint32_t kDopMarkerOdd = 0x05u << 16;
constexpr uint32_t kDopMarkerEven = 0xFAu << 16;

constexpr uint8_t reverse8_bits(uint8_t x) {
    x = static_cast<uint8_t>(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
    x = static_cast<uint8_t>(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = static_cast<uint8_t>(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    return x;
}

constexpr std::array<uint8_t, 256> make_reverse8_lut() {
    std::array<uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        lut[static_cast<size_t>(i)] = reverse8_bits(static_cast<uint8_t>(i));
    }
    return lut;
}

constexpr std::array<uint8_t, 256> kReverse8Lut = make_reverse8_lut();

/**
 * @brief Bit-reverses every byte in [src, src + n) into dst.
 *
 * DSD bit order flips (MSB-first source into an LSB-first sink or vice
 * versa) are the hottest per-byte operation in this file, so this is the
 * one spot worth a NEON path: RBIT.8B/16B reverses bits in 16 lanes per
 * instruction versus one byte per LUT load. src == dst is safe since the
 * transform is purely elementwise, no cross-byte dependency.
 */
void reverse_bits_bulk(const uint8_t *src, uint8_t *dst, size_t n) {
#if defined(DSD_PACKER_HAVE_NEON)
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        vst1q_u8(dst + i, vrbitq_u8(vld1q_u8(src + i)));
    }
    for (; i < n; ++i) {
        dst[i] = kReverse8Lut[src[i]];
    }
#else
    for (size_t i = 0; i < n; ++i) {
        dst[i] = kReverse8Lut[src[i]];
    }
#endif
}

bool is_le_dsd_format(tinyalsa::sample_format fmt) {
    switch (fmt) {
        case tinyalsa::sample_format::dsd_u8:
        case tinyalsa::sample_format::dsd_u16_le:
        case tinyalsa::sample_format::dsd_u32_le: return true;
        default: return false;
    }
}

} // namespace

DsdOutputMode choose_dsd_output_mode(
    const tinyalsa::pcm_params &params,
    tinyalsa::size_type dsdRate,
    bool allowNative,
    bool allowDop,
    tinyalsa::sample_format *outNativeFmt
) {
    if (allowNative) {
        constexpr std::array<tinyalsa::sample_format, 5> nativeFmts{
            tinyalsa::sample_format::dsd_u32_be,
            tinyalsa::sample_format::dsd_u32_le,
            tinyalsa::sample_format::dsd_u16_be,
            tinyalsa::sample_format::dsd_u16_le,
            tinyalsa::sample_format::dsd_u8
        };

        for (auto fmt : nativeFmts) {
            tinyalsa::size_type containerBytes = tinyalsa::bytes_per_frame(fmt, 1);
            if (containerBytes == 0) continue;
            // dsdRate is FFmpeg's dsf/dsdiff sample_rate, which is already the raw
            // DSD byte rate (bit rate / 8, one 8-bit DSD byte per "sample"). Each
            // native container packs containerBytes raw DSD bytes, so the container
            // rate is dsdRate / containerBytes, not dsdRate / (containerBytes * 8).
            tinyalsa::size_type nativeRate = dsdRate / containerBytes;
            if (params.test_config(2, nativeRate, fmt)) {
                if (outNativeFmt) *outNativeFmt = fmt;
                return DsdOutputMode::Native;
            }
        }
    }

    if (allowDop) {
        // DoP packs 2 raw DSD bytes (16 bits) per 24-bit PCM word, so the PCM word
        // rate is dsdRate / 2 given dsdRate is already a byte rate (see note above).
        tinyalsa::size_type dopRate = dsdRate / 2;
        if (params.test_config(2, dopRate, tinyalsa::sample_format::s24_le) ||
            params.test_config(2, dopRate, tinyalsa::sample_format::s32_le))
            return DsdOutputMode::Dop;
    }

    return DsdOutputMode::Pcm;
}

tinyalsa::sample_format pick_native_dsd_format(const tinyalsa::pcm_params &params) {
    constexpr std::array<tinyalsa::sample_format, 5> nativeFmts{
        tinyalsa::sample_format::dsd_u32_be,
        tinyalsa::sample_format::dsd_u32_le,
        tinyalsa::sample_format::dsd_u16_be,
        tinyalsa::sample_format::dsd_u16_le,
        tinyalsa::sample_format::dsd_u8
    };

    for (auto fmt : nativeFmts) {
        if (params.test_format(fmt)) return fmt;
    }

    return tinyalsa::sample_format::dsd_u8;
}

tinyalsa::size_type dsd_pcm_rate(DsdOutputMode mode, tinyalsa::size_type dsdRate, tinyalsa::sample_format nativeFmt) {
    switch (mode) {
        case DsdOutputMode::Native: {
            tinyalsa::size_type containerBytes = tinyalsa::bytes_per_frame(nativeFmt, 1);
            return containerBytes ? dsdRate / containerBytes : dsdRate;
        }
        case DsdOutputMode::Dop: return dsdRate / 2;
        default: return dsdRate;
    }
}

// ============================================================================
// DopPacker
// ============================================================================

DopPacker::DopPacker(const DsdSourceLayout &layout, bool wideningTo32Bit)
    : layout_(layout)
    , widening_(wideningTo32Bit) {
}

void DopPacker::reset() {
    markerOdd_ = true;
    pending_.clear();
}

size_t DopPacker::max_output_frames_for(size_t inBytes) const {
    const size_t channels = static_cast<size_t>(layout_.channels);
    if (channels == 0) return 0;

    const size_t total = pending_.size() + inBytes;

    if (layout_.planar) {
        const size_t groupBytes = static_cast<size_t>(layout_.block_size) * channels;
        if (groupBytes == 0) return 0;

        return (total / groupBytes) * (layout_.block_size / 2);
    }

    return total / (2 * channels);
}

size_t DopPacker::pack(const uint8_t *in, size_t inBytes, int32_t *out, size_t outCapacityFrames) {
    if (in && inBytes) {
        const size_t offset = pending_.size();
        pending_.resize(offset + inBytes);
        // Reverse once per appended chunk here instead of per byte in emit(),
        // so the bulk NEON path in reverse_bits_bulk actually gets a run of
        // bytes worth vectorizing rather than being called two bytes at a time.
        if (layout_.msb_first) {
            std::memcpy(pending_.data() + offset, in, inBytes);
        } else {
            reverse_bits_bulk(in, pending_.data() + offset, inBytes);
        }
    }

    const int channels = layout_.channels;
    size_t written = 0;

    auto emit = [&](uint8_t older, uint8_t newer, size_t frame, int channel) {
        uint32_t marker = markerOdd_ ? kDopMarkerOdd : kDopMarkerEven;
        uint32_t word24 = marker |
                          (static_cast<uint32_t>(older) << 8) |
                          static_cast<uint32_t>(newer);

        uint32_t word32;
        if (widening_) {
            word32 = word24 << 8;
        } else {
            word32 = word24;
        }

        int32_t sample;
        std::memcpy(&sample, &word32, sizeof(sample));
        out[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)] = sample;
    };

    if (layout_.planar) {
        const size_t groupBytes =
            static_cast<size_t>(layout_.block_size) * static_cast<size_t>(channels);
        const size_t framesPerGroup = layout_.block_size / 2;

        if (groupBytes == 0 || framesPerGroup == 0) return 0;

        const size_t groupsAvail = pending_.size() / groupBytes;
        const size_t groupsCap = (outCapacityFrames - written) / framesPerGroup;
        const size_t groups = std::min(groupsAvail, groupsCap);

        if (groups > 0) {
            const uint8_t *base = pending_.data();

            for (size_t g = 0; g < groups; ++g) {
                const uint8_t *group = base + g * groupBytes;

                for (size_t k = 0; k < framesPerGroup; ++k) {
                    for (int c = 0; c < channels; ++c) {
                        const uint8_t *block =
                            group + static_cast<size_t>(c) * layout_.block_size;

                        emit(block[2 * k], block[2 * k + 1], written + k, c);
                    }

                    markerOdd_ = !markerOdd_;
                }

                written += framesPerGroup;
            }

            pending_.erase(
                pending_.begin(),
                pending_.begin() + static_cast<std::ptrdiff_t>(groups * groupBytes)
            );
        }

        return written;
    }

    const size_t pairBytes = 2 * static_cast<size_t>(channels);
    if (pairBytes == 0) return 0;

    const size_t framesAvail = pending_.size() / pairBytes;
    const size_t frames = std::min(framesAvail, outCapacityFrames - written);

    if (frames > 0) {
        const uint8_t *base = pending_.data();

        for (size_t k = 0; k < frames; ++k) {
            const uint8_t *first = base + (2 * k) * static_cast<size_t>(channels);
            const uint8_t *second = base + (2 * k + 1) * static_cast<size_t>(channels);

            for (int c = 0; c < channels; ++c) {
                emit(first[c], second[c], written + k, c);
            }

            markerOdd_ = !markerOdd_;
        }

        pending_.erase(
            pending_.begin(),
            pending_.begin() + static_cast<std::ptrdiff_t>(frames * pairBytes)
        );

        written += frames;
    }

    return written;
}

// ============================================================================
// NativeDsdPacker
// ============================================================================

NativeDsdPacker::NativeDsdPacker(const DsdSourceLayout &layout, tinyalsa::sample_format target)
    : layout_(layout)
    , target_(target) {
}

size_t NativeDsdPacker::pack(const uint8_t *in, size_t inBytes, uint8_t *out, size_t outCapacityBytes) {
    const int channels = layout_.channels;
    const size_t containerBytes = static_cast<size_t>(tinyalsa::bytes_per_frame(target_, 1));
    if (containerBytes == 0) return 0;

    const bool le = is_le_dsd_format(target_);
    const size_t outFrameBytes = static_cast<size_t>(channels) * containerBytes;

    // Bit-reverse the whole input once up front (one bulk NEON pass) instead
    // of per container byte inside the gather loops below, since the
    // reversal itself doesn't depend on the planar/interleaved layout.
    const uint8_t *src = in;
    if (!layout_.msb_first) {
        scratch_.resize(inBytes);
        reverse_bits_bulk(in, scratch_.data(), inBytes);
        src = scratch_.data();
    }

    auto writeContainer = [&](const uint8_t *raw, uint8_t *dst) {
        if (le) {
            for (size_t b = 0; b < containerBytes; ++b) dst[containerBytes - 1 - b] = raw[b];
        } else {
            std::memcpy(dst, raw, containerBytes);
        }
    };

    if (layout_.planar) {
        if (inBytes < layout_.block_size * static_cast<size_t>(channels)) return 0;

        size_t groups = layout_.block_size / containerBytes;
        size_t frames = std::min(groups, outCapacityBytes / outFrameBytes);

        for (size_t k = 0; k < frames; ++k) {
            for (int c = 0; c < channels; ++c) {
                const uint8_t *block = src + static_cast<size_t>(c) * layout_.block_size;
                writeContainer(block + k * containerBytes, out + (k * channels + c) * containerBytes);
            }
        }
        return frames * outFrameBytes;
    }

    size_t byteFramesAvail = inBytes / static_cast<size_t>(channels);
    size_t groupsAvail = byteFramesAvail / containerBytes;
    size_t frames = std::min(groupsAvail, outCapacityBytes / outFrameBytes);

    std::vector<uint8_t> raw(containerBytes);
    for (size_t k = 0; k < frames; ++k) {
        for (int c = 0; c < channels; ++c) {
            for (size_t b = 0; b < containerBytes; ++b) {
                raw[b] = src[(k * containerBytes + b) * channels + c];
            }
            writeContainer(raw.data(), out + (k * channels + c) * containerBytes);
        }
    }
    return frames * outFrameBytes;
}
