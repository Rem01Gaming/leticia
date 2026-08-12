#include "DsdPacker.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace {

constexpr int32_t kDopMarkerOdd = 0x00050000;
constexpr int32_t kDopMarkerEven = 0xFFFA0000;

uint8_t reverse8(uint8_t x) {
    x = static_cast<uint8_t>(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
    x = static_cast<uint8_t>(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = static_cast<uint8_t>(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    return x;
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
    const tinyalsa::pcm_params &params, tinyalsa::size_type dsdRate, bool allowNative, bool allowDop,
    tinyalsa::sample_format *outNativeFmt
) {
    if (allowNative) {
        constexpr std::array<tinyalsa::sample_format, 5> nativeFmts{
            tinyalsa::sample_format::dsd_u32_be, tinyalsa::sample_format::dsd_u32_le, tinyalsa::sample_format::dsd_u16_be,
            tinyalsa::sample_format::dsd_u16_le, tinyalsa::sample_format::dsd_u8
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
        tinyalsa::sample_format::dsd_u32_be, tinyalsa::sample_format::dsd_u32_le, tinyalsa::sample_format::dsd_u16_be,
        tinyalsa::sample_format::dsd_u16_le, tinyalsa::sample_format::dsd_u8
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

DopPacker::DopPacker(const DsdSourceLayout &layout, bool wideningTo32Bit) : layout_(layout), widening_(wideningTo32Bit) {
}

void DopPacker::reset() {
    markerOdd_ = true;
}

size_t DopPacker::pack(const uint8_t *in, size_t inBytes, int32_t *out, size_t outCapacityFrames) {
    const int channels = layout_.channels;

    auto emit = [&](uint8_t older, uint8_t newer, size_t frame, int channel) {
        if (layout_.msb_first) {
            older = reverse8(older);
            newer = reverse8(newer);
        }
        int32_t marker = markerOdd_ ? kDopMarkerOdd : kDopMarkerEven;
        int32_t sample = marker | (static_cast<int32_t>(older) << 8) | static_cast<int32_t>(newer);
        if (widening_) sample <<= 8;
        out[frame * channels + channel] = sample;
    };

    if (layout_.planar) {
        if (inBytes < layout_.block_size * static_cast<size_t>(channels)) return 0;

        size_t pairsPerBlock = layout_.block_size / 2;
        size_t frames = std::min(pairsPerBlock, outCapacityFrames);

        for (size_t k = 0; k < frames; ++k) {
            for (int c = 0; c < channels; ++c) {
                const uint8_t *block = in + static_cast<size_t>(c) * layout_.block_size;
                emit(block[2 * k], block[2 * k + 1], k, c);
            }
            markerOdd_ = !markerOdd_;
        }
        return frames;
    }

    size_t byteFramesAvail = inBytes / static_cast<size_t>(channels);
    size_t pairsAvail = byteFramesAvail / 2;
    size_t frames = std::min(pairsAvail, outCapacityFrames);

    for (size_t k = 0; k < frames; ++k) {
        const uint8_t *first = in + (2 * k) * channels;
        const uint8_t *second = in + (2 * k + 1) * channels;
        for (int c = 0; c < channels; ++c) {
            emit(first[c], second[c], k, c);
        }
        markerOdd_ = !markerOdd_;
    }
    return frames;
}

// ============================================================================
// NativeDsdPacker
// ============================================================================

NativeDsdPacker::NativeDsdPacker(const DsdSourceLayout &layout, tinyalsa::sample_format target) : layout_(layout), target_(target) {
}

size_t NativeDsdPacker::pack(const uint8_t *in, size_t inBytes, uint8_t *out, size_t outCapacityBytes) {
    const int channels = layout_.channels;
    const size_t containerBytes = static_cast<size_t>(tinyalsa::bytes_per_frame(target_, 1));
    if (containerBytes == 0) return 0;

    const bool le = is_le_dsd_format(target_);
    const bool needReverse = !layout_.msb_first;
    const size_t outFrameBytes = static_cast<size_t>(channels) * containerBytes;

    auto writeContainer = [&](const uint8_t *raw, uint8_t *dst) {
        for (size_t b = 0; b < containerBytes; ++b) {
            uint8_t v = needReverse ? reverse8(raw[b]) : raw[b];
            dst[le ? (containerBytes - 1 - b) : b] = v;
        }
    };

    if (layout_.planar) {
        if (inBytes < layout_.block_size * static_cast<size_t>(channels)) return 0;

        size_t groups = layout_.block_size / containerBytes;
        size_t frames = std::min(groups, outCapacityBytes / outFrameBytes);

        for (size_t k = 0; k < frames; ++k) {
            for (int c = 0; c < channels; ++c) {
                const uint8_t *block = in + static_cast<size_t>(c) * layout_.block_size;
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
                raw[b] = in[(k * containerBytes + b) * channels + c];
            }
            writeContainer(raw.data(), out + (k * channels + c) * containerBytes);
        }
    }
    return frames * outFrameBytes;
}
