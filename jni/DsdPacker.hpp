/**
 * DsdPacker.hpp - Raw DSD bitstream repacking for DoP and native DSD playback
 *
 * FFmpeg's dsd_lsbf/dsd_msbf decoders convert DSD to PCM in software (the
 * existing playback path in Main.cpp). This module instead repacks the raw,
 * undecoded DSD bytes straight out of the demuxer so the bitstream reaches
 * the DAC unmodified, either wrapped as DoP (DSD over PCM) or written using
 * ALSA's native DSD_U* sample formats.
 */

#pragma once

#include "TinyAlsa.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Output strategy for a DSD source.
 */
enum class DsdOutputMode {
    Pcm,   ///< Software DSD to PCM conversion (existing FFmpeg decoder path).
    Dop,   ///< DSD over PCM: raw DSD bits framed inside 24-bit PCM words.
    Native ///< Bit-perfect passthrough using an ALSA native DSD_U* format.
};

/**
 * @brief Describes how raw DSD bytes are laid out inside one demuxed AVPacket.
 */
struct DsdSourceLayout {
    int channels = 2;
    bool msb_first = true;    ///< True for *_MSBF codecs (e.g. DSDIFF), false for *_LSBF (e.g. DSF).
    bool planar = false;      ///< True for *_PLANAR codecs: fixed-size per-channel blocks (DSF-style).
    size_t block_size = 4096; ///< Bytes per channel per block, only used when planar is true.
};

/**
 * @brief Picks the best DSD output strategy the hardware actually supports.
 * @param params       An already-open pcm_params probe for the target device.
 * @param dsdRate      DSD bit rate per channel in Hz, e.g. 2822400 for DSD64.
 * @param allowNative  Whether native DSD_U* passthrough may be considered.
 * @param allowDop     Whether DoP framing may be considered.
 * @param outNativeFmt When non-null and this returns DsdOutputMode::Native, set
 * to the specific DSD_U* format that was jointly validated (channels+rate+format
 * together) for this device. Callers configuring Native mode should use this
 * value rather than re-deriving it, since a separate lookup could pick a
 * different format that only passes a weaker, single-axis capability check.
 * @note Native is preferred over DoP whenever both are supported, since it
 * avoids the PCM-container framing overhead entirely.
 */
DsdOutputMode choose_dsd_output_mode(
    const tinyalsa::pcm_params &params,
    tinyalsa::size_type dsdRate,
    bool allowNative,
    bool allowDop,
    tinyalsa::sample_format *outNativeFmt = nullptr
);

/**
 * @brief Returns the best-supported tinyalsa native DSD sample_format, widest
 * container first, using a single-axis format check only.
 * @deprecated Prefer the outNativeFmt populated by choose_dsd_output_mode(),
 * which validates channels+rate+format jointly. This function can disagree
 * with that joint check on hardware where a format is only valid at certain
 * rates, since it never considers rate at all.
 */
tinyalsa::sample_format pick_native_dsd_format(const tinyalsa::pcm_params &params);

/**
 * @brief Returns the PCM sample rate ALSA must be configured with for a given
 * DSD output mode, derived from the raw DSD bit rate.
 */
tinyalsa::size_type dsd_pcm_rate(DsdOutputMode mode, tinyalsa::size_type dsdRate, tinyalsa::sample_format nativeFmt);

// ============================================================================
// DopPacker
// ============================================================================

/**
 * @brief Packs raw DSD bitstream bytes into DoP-framed PCM words.
 *
 * Two raw DSD bytes per channel (16 bits, the period over which the DoP
 * marker alternates) become one interleaved PCM sample per channel: the top
 * byte is the alternating 0x05 / 0xFA sync marker, followed by the two raw
 * DSD bytes. MSB-first sources are bit-reversed first, since DoP's wire
 * format expects LSB-first byte order.
 */
class DopPacker {
public:
    /**
     * @param layout     Source byte layout, see DsdSourceLayout.
     * @param wideningTo32Bit Set true when the ALSA format is S32_LE (full
     * 32-bit container) rather than S24_LE, left-justifying the 24-bit DoP
     * word into the top 3 bytes instead of the bottom 3.
     */
    explicit DopPacker(const DsdSourceLayout &layout, bool wideningTo32Bit = false);

    /**
     * @brief Resets the DoP marker state. Call this after a seek to prevent
     * audio glitches caused by marker phase misalignment.
     */
    void reset();

    /**
     * @brief Returns the maximum number of output frames that could be produced
     *        if @p inBytes additional input bytes are appended to the internal
     *        carry-over buffer.
     */
    size_t max_output_frames_for(size_t inBytes) const;

    /**
     * @brief Packs one full demuxed packet (or one planar block group) of raw DSD bytes.
     * @param in                Raw packet bytes exactly as read from the demuxer.
     * @param inBytes           Size of @p in in bytes.
     * @param out               Destination buffer, interleaved by channel.
     * @param outCapacityFrames Capacity of @p out in frames (channels int32_t entries each).
     * @return Number of output frames written.
     */
    size_t pack(const uint8_t *in, size_t inBytes, int32_t *out, size_t outCapacityFrames);

    /**
     * @brief Packs DoP frames into packed S24_3LE little-endian samples.
     * @param in                Raw packet bytes exactly as read from the demuxer.
     * @param inBytes           Size of @p in in bytes.
     * @param out               Destination buffer, interleaved by channel.
     * @param outCapacityFrames Capacity of @p out in frames (channels int32_t entries each).
     * @return Number of output frames written.
     */
    size_t pack24(const uint8_t *in, size_t inBytes, uint8_t *out, size_t outCapacityFrames);

private:
    DsdSourceLayout layout_;
    bool widening_;
    bool markerOdd_ = true;
    std::vector<uint8_t> pending_;
};

// ============================================================================
// NativeDsdPacker
// ============================================================================

/**
 * @brief Repacks raw DSD bitstream bytes into an ALSA native DSD_U* container,
 * fixing up bit order and channel interleaving to match the target format.
 *
 * @note Native DSD_U* byte-order conventions are not formally standardized
 * and differ across drivers. This class follows the common convention (used
 * by most XMOS/Amanero-class USB DACs) where DSD_U*_BE formats expect
 * MSB-first bytes in arrival order and DSD_U*_LE formats expect the same
 * bytes reversed. If a specific DAC stays silent or produces noise, the
 * fix is usually to try the other endianness of the same container width.
 */
class NativeDsdPacker {
public:
    NativeDsdPacker(const DsdSourceLayout &layout, tinyalsa::sample_format target);

    /**
     * @brief Packs one full demuxed packet (or one planar block group) of raw DSD bytes.
     * @param in               Raw packet bytes exactly as read from the demuxer.
     * @param inBytes          Size of @p in in bytes.
     * @param out              Destination buffer, interleaved by channel.
     * @param outCapacityBytes Capacity of @p out in bytes.
     * @return Number of bytes written to @p out.
     */
    size_t pack(const uint8_t *in, size_t inBytes, uint8_t *out, size_t outCapacityBytes);

private:
    DsdSourceLayout layout_;
    tinyalsa::sample_format target_;
    std::vector<uint8_t> scratch_;
};
