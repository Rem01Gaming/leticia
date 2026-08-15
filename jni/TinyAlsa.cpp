#include "TinyAlsa.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sound/asound.h>
#include <sound/tlv.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#else
#define UNLIKELY(cond) (cond)
#endif

namespace tinyalsa {

// ============================================================================
// hw_params mask / interval helpers
// ============================================================================

namespace {

using parameter_name = int;

template <parameter_name param>
struct mask_ref final {
    using value_type = std::remove_reference<decltype(snd_mask::bits[0])>::type;

    static constexpr bool out_of_range() noexcept {
        return (param < SNDRV_PCM_HW_PARAM_FIRST_MASK) || (param > SNDRV_PCM_HW_PARAM_LAST_MASK);
    }

    static constexpr void init(snd_pcm_hw_params &hw_params) noexcept {
        static_assert(!out_of_range(), "Not a mask parameter.");
        auto &mask = hw_params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        mask.bits[0] = std::numeric_limits<value_type>::max();
        mask.bits[1] = std::numeric_limits<value_type>::max();
    }

    static constexpr void set(snd_pcm_hw_params &hw_params, value_type value) noexcept {
        static_assert(!out_of_range(), "Not a mask parameter.");
        auto &mask = hw_params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        mask.bits[0] = 0;
        mask.bits[1] = 0;
        mask.bits[value >> 5] |= (1u << (value & 31));
    }

    static constexpr bool test(const snd_pcm_hw_params &hw_params, value_type value) noexcept {
        static_assert(!out_of_range(), "Not a mask parameter.");
        const auto &mask = hw_params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        return !!(mask.bits[value >> 5] & (1u << (value & 31)));
    }
};

template <parameter_name param = SNDRV_PCM_HW_PARAM_FIRST_MASK>
struct masks_initializer final {
    static constexpr void init(snd_pcm_hw_params &hw_params) noexcept {
        mask_ref<param>::init(hw_params);
        masks_initializer<param + 1>::init(hw_params);
    }
};

template <>
struct masks_initializer<SNDRV_PCM_HW_PARAM_LAST_MASK + 1> final {
    static constexpr void init(snd_pcm_hw_params &) noexcept {
    }
};

} // namespace

namespace {

template <parameter_name name>
struct interval_ref final {
    using value_type = decltype(snd_interval::min);

    static constexpr bool out_of_range() noexcept {
        return (name < SNDRV_PCM_HW_PARAM_FIRST_INTERVAL) || (name > SNDRV_PCM_HW_PARAM_LAST_INTERVAL);
    }

    static constexpr void set(snd_pcm_hw_params &hw_params, value_type value) noexcept {
        static_assert(!out_of_range(), "Not an interval parameter.");
        auto &ref = hw_params.intervals[name - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        ref.min = value;
        ref.max = value;
        ref.integer = 1;
    }

    static constexpr void init(snd_pcm_hw_params &hw_params) noexcept {
        static_assert(!out_of_range(), "Not an interval parameter.");
        auto &ref = hw_params.intervals[name - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        ref.max = std::numeric_limits<value_type>::max();
    }
};

template <parameter_name name = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL>
struct intervals_initializer final {
    inline static constexpr void init(snd_pcm_hw_params &hw_params) noexcept {
        interval_ref<name>::init(hw_params);
        intervals_initializer<name + 1>::init(hw_params);
    }
};

template <>
struct intervals_initializer<SNDRV_PCM_HW_PARAM_LAST_INTERVAL + 1> final {
    inline static constexpr void init(snd_pcm_hw_params &) noexcept {
    }
};

} // namespace

inline constexpr snd_pcm_hw_params init_hw_parameters() noexcept {
    snd_pcm_hw_params params{};
    masks_initializer<>::init(params);
    intervals_initializer<>::init(params);
    params.rmask = ~0U;
    params.info = ~0U;
    return params;
}

// ============================================================================
// type converters
// ============================================================================

namespace {

auto to_tinyalsa_class(int native_class) noexcept {
    switch (native_class) {
        case SNDRV_PCM_CLASS_GENERIC: return pcm_class::generic;
        case SNDRV_PCM_CLASS_MULTI: return pcm_class::multi_channel;
        case SNDRV_PCM_CLASS_MODEM: return pcm_class::modem;
        case SNDRV_PCM_CLASS_DIGITIZER: return pcm_class::digitizer;
        default: break;
    }
    return pcm_class::unknown;
}

auto to_tinyalsa_subclass(int native_subclass) noexcept {
    switch (native_subclass) {
        case SNDRV_PCM_SUBCLASS_GENERIC_MIX: return pcm_subclass::generic_mix;
        case SNDRV_PCM_SUBCLASS_MULTI_MIX: return pcm_subclass::multi_channel_mix;
        default: break;
    }
    return pcm_subclass::unknown;
}

tinyalsa::pcm_info to_tinyalsa_info(const snd_pcm_info &native_info) noexcept {
    tinyalsa::pcm_info out;
    out.device = native_info.device;
    out.subdevice = native_info.subdevice;
    out.card = native_info.card;
    out.subdevices_count = native_info.subdevices_count;
    out.subdevices_available = native_info.subdevices_avail;
    memcpy(out.id, native_info.id, std::min(sizeof(out.id), sizeof(native_info.id)));
    memcpy(out.name, native_info.name, std::min(sizeof(out.name), sizeof(native_info.name)));
    memcpy(out.subname, native_info.subname, std::min(sizeof(out.subname), sizeof(native_info.subname)));
    out.class_ = to_tinyalsa_class(native_info.dev_class);
    out.subclass = to_tinyalsa_subclass(native_info.dev_subclass);
    return out;
}

pcm_state to_tinyalsa_state(snd_pcm_state_t s) noexcept {
    switch (s) {
        case SNDRV_PCM_STATE_OPEN: return pcm_state::open;
        case SNDRV_PCM_STATE_SETUP: return pcm_state::setup;
        case SNDRV_PCM_STATE_PREPARED: return pcm_state::prepared;
        case SNDRV_PCM_STATE_RUNNING: return pcm_state::running;
        case SNDRV_PCM_STATE_XRUN: return pcm_state::xrun;
        case SNDRV_PCM_STATE_DRAINING: return pcm_state::draining;
        case SNDRV_PCM_STATE_PAUSED: return pcm_state::paused;
        case SNDRV_PCM_STATE_SUSPENDED: return pcm_state::suspended;
        case SNDRV_PCM_STATE_DISCONNECTED: return pcm_state::disconnected;
        default: break;
    }
    return pcm_state::disconnected;
}

} // namespace

// ============================================================================
// channel_position
// ============================================================================

channel_position to_channel_position(unsigned int raw_chmap_value) noexcept {
    switch (raw_chmap_value & SNDRV_CHMAP_POSITION_MASK) {
        case SNDRV_CHMAP_NA: return channel_position::na;
        case SNDRV_CHMAP_MONO: return channel_position::mono;
        case SNDRV_CHMAP_FL: return channel_position::front_left;
        case SNDRV_CHMAP_FR: return channel_position::front_right;
        case SNDRV_CHMAP_RL: return channel_position::rear_left;
        case SNDRV_CHMAP_RR: return channel_position::rear_right;
        case SNDRV_CHMAP_FC: return channel_position::front_center;
        case SNDRV_CHMAP_LFE: return channel_position::lfe;
        case SNDRV_CHMAP_SL: return channel_position::side_left;
        case SNDRV_CHMAP_SR: return channel_position::side_right;
        case SNDRV_CHMAP_RC: return channel_position::rear_center;
        case SNDRV_CHMAP_FLC: return channel_position::front_left_center;
        case SNDRV_CHMAP_FRC: return channel_position::front_right_center;
        case SNDRV_CHMAP_RLC: return channel_position::rear_left_center;
        case SNDRV_CHMAP_RRC: return channel_position::rear_right_center;
        case SNDRV_CHMAP_FLW: return channel_position::front_left_wide;
        case SNDRV_CHMAP_FRW: return channel_position::front_right_wide;
        case SNDRV_CHMAP_FLH: return channel_position::front_left_high;
        case SNDRV_CHMAP_FCH: return channel_position::front_center_high;
        case SNDRV_CHMAP_FRH: return channel_position::front_right_high;
        case SNDRV_CHMAP_TC: return channel_position::top_center;
        case SNDRV_CHMAP_TFL: return channel_position::top_front_left;
        case SNDRV_CHMAP_TFR: return channel_position::top_front_right;
        case SNDRV_CHMAP_TFC: return channel_position::top_front_center;
        case SNDRV_CHMAP_TRL: return channel_position::top_rear_left;
        case SNDRV_CHMAP_TRR: return channel_position::top_rear_right;
        case SNDRV_CHMAP_TRC: return channel_position::top_rear_center;
        case SNDRV_CHMAP_TFLC: return channel_position::top_front_left_center;
        case SNDRV_CHMAP_TFRC: return channel_position::top_front_right_center;
        case SNDRV_CHMAP_TSL: return channel_position::top_side_left;
        case SNDRV_CHMAP_TSR: return channel_position::top_side_right;
        case SNDRV_CHMAP_LLFE: return channel_position::left_lfe;
        case SNDRV_CHMAP_RLFE: return channel_position::right_lfe;
        case SNDRV_CHMAP_BC: return channel_position::bottom_center;
        case SNDRV_CHMAP_BLC: return channel_position::bottom_left_center;
        case SNDRV_CHMAP_BRC: return channel_position::bottom_right_center;
        default: break;
    }
    return channel_position::unknown;
}

const char *to_string(channel_position pos) noexcept {
    switch (pos) {
        case channel_position::unknown: return "UNKNOWN";
        case channel_position::na: return "NA";
        case channel_position::mono: return "MONO";
        case channel_position::front_left: return "FL";
        case channel_position::front_right: return "FR";
        case channel_position::rear_left: return "RL";
        case channel_position::rear_right: return "RR";
        case channel_position::front_center: return "FC";
        case channel_position::lfe: return "LFE";
        case channel_position::side_left: return "SL";
        case channel_position::side_right: return "SR";
        case channel_position::rear_center: return "RC";
        case channel_position::front_left_center: return "FLC";
        case channel_position::front_right_center: return "FRC";
        case channel_position::rear_left_center: return "RLC";
        case channel_position::rear_right_center: return "RRC";
        case channel_position::front_left_wide: return "FLW";
        case channel_position::front_right_wide: return "FRW";
        case channel_position::front_left_high: return "FLH";
        case channel_position::front_center_high: return "FCH";
        case channel_position::front_right_high: return "FRH";
        case channel_position::top_center: return "TC";
        case channel_position::top_front_left: return "TFL";
        case channel_position::top_front_right: return "TFR";
        case channel_position::top_front_center: return "TFC";
        case channel_position::top_rear_left: return "TRL";
        case channel_position::top_rear_right: return "TRR";
        case channel_position::top_rear_center: return "TRC";
        case channel_position::top_front_left_center: return "TFLC";
        case channel_position::top_front_right_center: return "TFRC";
        case channel_position::top_side_left: return "TSL";
        case channel_position::top_side_right: return "TSR";
        case channel_position::left_lfe: return "LLFE";
        case channel_position::right_lfe: return "RLFE";
        case channel_position::bottom_center: return "BC";
        case channel_position::bottom_left_center: return "BLC";
        case channel_position::bottom_right_center: return "BRC";
    }
    return "UNKNOWN";
}

namespace {

constexpr int to_alsa_format(sample_format sf) noexcept {
    switch (sf) {
        case sample_format::u8: return SNDRV_PCM_FORMAT_U8;
        case sample_format::u16_le: return SNDRV_PCM_FORMAT_U16_LE;
        case sample_format::u16_be: return SNDRV_PCM_FORMAT_U16_BE;
        case sample_format::u18_3le: return SNDRV_PCM_FORMAT_U18_3LE;
        case sample_format::u18_3be: return SNDRV_PCM_FORMAT_U18_3BE;
        case sample_format::u20_3le: return SNDRV_PCM_FORMAT_U20_3LE;
        case sample_format::u20_3be: return SNDRV_PCM_FORMAT_U20_3BE;
        case sample_format::u24_3le: return SNDRV_PCM_FORMAT_U24_3LE;
        case sample_format::u24_3be: return SNDRV_PCM_FORMAT_U24_3BE;
        case sample_format::u24_le: return SNDRV_PCM_FORMAT_U24_LE;
        case sample_format::u24_be: return SNDRV_PCM_FORMAT_U24_BE;
        case sample_format::u32_le: return SNDRV_PCM_FORMAT_U32_LE;
        case sample_format::u32_be: return SNDRV_PCM_FORMAT_U32_BE;
        case sample_format::s8: return SNDRV_PCM_FORMAT_S8;
        case sample_format::s16_le: return SNDRV_PCM_FORMAT_S16_LE;
        case sample_format::s16_be: return SNDRV_PCM_FORMAT_S16_BE;
        case sample_format::s18_3le: return SNDRV_PCM_FORMAT_S18_3LE;
        case sample_format::s18_3be: return SNDRV_PCM_FORMAT_S18_3BE;
        case sample_format::s20_3le: return SNDRV_PCM_FORMAT_S20_3LE;
        case sample_format::s20_3be: return SNDRV_PCM_FORMAT_S20_3BE;
        case sample_format::s24_3le: return SNDRV_PCM_FORMAT_S24_3LE;
        case sample_format::s24_3be: return SNDRV_PCM_FORMAT_S24_3BE;
        case sample_format::s24_le: return SNDRV_PCM_FORMAT_S24_LE;
        case sample_format::s24_be: return SNDRV_PCM_FORMAT_S24_BE;
        case sample_format::s32_le: return SNDRV_PCM_FORMAT_S32_LE;
        case sample_format::s32_be: return SNDRV_PCM_FORMAT_S32_BE;
        case sample_format::dsd_u8: return SNDRV_PCM_FORMAT_DSD_U8;
        case sample_format::dsd_u16_le: return SNDRV_PCM_FORMAT_DSD_U16_LE;
        case sample_format::dsd_u16_be: return SNDRV_PCM_FORMAT_DSD_U16_BE;
        case sample_format::dsd_u32_le: return SNDRV_PCM_FORMAT_DSD_U32_LE;
        case sample_format::dsd_u32_be: return SNDRV_PCM_FORMAT_DSD_U32_BE;
    }
    return 0;
}

constexpr int to_alsa_access(sample_access access) noexcept {
    switch (access) {
        case sample_access::interleaved: return SNDRV_PCM_ACCESS_RW_INTERLEAVED;
        case sample_access::non_interleaved: return SNDRV_PCM_ACCESS_RW_NONINTERLEAVED;
        case sample_access::mmap_interleaved: return SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
        case sample_access::mmap_non_interleaved: return SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED;
    }
    return 0;
}

constexpr snd_pcm_hw_params to_alsa_hw_params(const pcm_config &config, sample_access access) noexcept {
    auto params = init_hw_parameters();
    interval_ref<SNDRV_PCM_HW_PARAM_CHANNELS>::set(params, config.channels);
    interval_ref<SNDRV_PCM_HW_PARAM_PERIOD_SIZE>::set(params, config.period_size);
    interval_ref<SNDRV_PCM_HW_PARAM_PERIODS>::set(params, config.period_count);
    interval_ref<SNDRV_PCM_HW_PARAM_RATE>::set(params, config.rate);
    mask_ref<SNDRV_PCM_HW_PARAM_FORMAT>::set(params, to_alsa_format(config.format));
    mask_ref<SNDRV_PCM_HW_PARAM_ACCESS>::set(params, to_alsa_access(access));
    if (config.disable_resampling) params.flags |= SNDRV_PCM_HW_PARAMS_NORESAMPLE;
    if (config.disable_period_wakeup) params.flags |= SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP;
    return params;
}

constexpr size_type compute_boundary(size_type buffer_size) noexcept {
    size_type boundary = buffer_size;
    while (boundary * 2 <= static_cast<size_type>(LONG_MAX) - buffer_size) boundary *= 2;
    return boundary;
}

constexpr snd_pcm_sw_params to_alsa_sw_params(const pcm_config &config, bool is_capture) noexcept {
    snd_pcm_sw_params params{};
    params.period_step = 1;
    params.avail_min = config.period_size;

    if (config.start_threshold) {
        params.start_threshold = config.start_threshold;
    } else if (is_capture) {
        params.start_threshold = 1;
    } else {
        params.start_threshold = config.period_count * config.period_size / 2;
    }

    if (config.stop_threshold) {
        params.stop_threshold = config.stop_threshold;
    } else if (is_capture) {
        params.stop_threshold = config.period_count * config.period_size * 10;
    } else {
        params.stop_threshold = config.period_count * config.period_size;
    }

    params.boundary = compute_boundary(config.period_count * config.period_size);

    params.xfer_align = config.period_size / 2;
    params.silence_size = 0;
    params.silence_threshold = config.silence_threshold;
    if (config.enable_timestamps) {
        params.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
        switch (config.tstamp_clock) {
            case pcm_config::timestamp_clock::monotonic: params.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC; break;
            case pcm_config::timestamp_clock::monotonic_raw: params.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW; break;
            case pcm_config::timestamp_clock::realtime: default: params.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY; break;
        }
    }
    return params;
}

} // namespace

// ============================================================================
// get_error_description / to_string / bytes_per_frame
// ============================================================================

const char *get_error_description(int error) noexcept {
    if (error == 0) return "Success";
    return ::strerror(error);
}

const char *to_string(sample_format sf) noexcept {
    switch (sf) {
        case sample_format::s8: return "S8";
        case sample_format::s16_le: return "S16_LE";
        case sample_format::s16_be: return "S16_BE";
        case sample_format::s18_3le: return "S18_3LE";
        case sample_format::s18_3be: return "S18_3BE";
        case sample_format::s20_3le: return "S20_3LE";
        case sample_format::s20_3be: return "S20_3BE";
        case sample_format::s24_3le: return "S24_3LE";
        case sample_format::s24_3be: return "S24_3BE";
        case sample_format::s24_le: return "S24_LE";
        case sample_format::s24_be: return "S24_BE";
        case sample_format::s32_le: return "S32_LE";
        case sample_format::s32_be: return "S32_BE";
        case sample_format::u8: return "U8";
        case sample_format::u16_le: return "U16_LE";
        case sample_format::u16_be: return "U16_BE";
        case sample_format::u18_3le: return "U18_3LE";
        case sample_format::u18_3be: return "U18_3BE";
        case sample_format::u20_3le: return "U20_3LE";
        case sample_format::u20_3be: return "U20_3BE";
        case sample_format::u24_3le: return "U24_3LE";
        case sample_format::u24_3be: return "U24_3BE";
        case sample_format::u24_le: return "U24_LE";
        case sample_format::u24_be: return "U24_BE";
        case sample_format::u32_le: return "U32_LE";
        case sample_format::u32_be: return "U32_BE";
        case sample_format::dsd_u8: return "DSD_U8";
        case sample_format::dsd_u16_le: return "DSD_U16_LE";
        case sample_format::dsd_u16_be: return "DSD_U16_BE";
        case sample_format::dsd_u32_le: return "DSD_U32_LE";
        case sample_format::dsd_u32_be: return "DSD_U32_BE";
    }
    return "UNKNOWN";
}

size_type bytes_per_frame(sample_format fmt, size_type channels) noexcept {
    size_type bps = 0;
    switch (fmt) {
        case sample_format::s8:
        case sample_format::u8: bps = 1; break;

        case sample_format::s16_le:
        case sample_format::s16_be:
        case sample_format::u16_le:
        case sample_format::u16_be: bps = 2; break;

        case sample_format::s18_3le:
        case sample_format::s18_3be:
        case sample_format::s20_3le:
        case sample_format::s20_3be:
        case sample_format::s24_3le:
        case sample_format::s24_3be:
        case sample_format::u18_3le:
        case sample_format::u18_3be:
        case sample_format::u20_3le:
        case sample_format::u20_3be:
        case sample_format::u24_3le:
        case sample_format::u24_3be: bps = 3; break;

        case sample_format::s24_le:
        case sample_format::s24_be:
        case sample_format::u24_le:
        case sample_format::u24_be:
        case sample_format::s32_le:
        case sample_format::s32_be:
        case sample_format::u32_le:
        case sample_format::u32_be: bps = 4; break;

        case sample_format::dsd_u8: bps = 1; break;

        case sample_format::dsd_u16_le:
        case sample_format::dsd_u16_be: bps = 2; break;

        case sample_format::dsd_u32_le:
        case sample_format::dsd_u32_be: bps = 4; break;
    }
    return bps * channels;
}

// ============================================================================
// pod_buffer
// ============================================================================

template <typename element_type>
struct pod_buffer final {
    static_assert(std::is_trivially_copyable_v<element_type>, "pod_buffer requires trivially copyable types");

    element_type *data = nullptr;
    size_type size = 0;
    size_type capacity = 0;

    constexpr pod_buffer() noexcept
        : data(nullptr)
        , size(0)
        , capacity(0) {
    }

    inline constexpr pod_buffer(pod_buffer &&other) noexcept
        : data(other.data)
        , size(other.size)
        , capacity(other.capacity) {
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
    }

    ~pod_buffer() {
        free(data);
        data = nullptr;
        size = 0;
        capacity = 0;
    }

    bool emplace_back(element_type &&e) noexcept {
        if (size == capacity) {
            size_type new_cap = capacity ? capacity * 2 : 8;
            auto *tmp = static_cast<element_type *>(realloc(data, new_cap * sizeof(element_type)));
            if (!tmp) return false;
            data = tmp;
            capacity = new_cap;
        }
        data[size++] = std::move(e);
        return true;
    }
};

// ============================================================================
// Interleaved reader
// ============================================================================

result interleaved_pcm_reader::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_capture_device(card, device, non_blocking);
}

generic_result<size_type> interleaved_pcm_reader::read_unformatted(void *frames, size_type frame_count) noexcept {
    snd_xferi transfer{0, frames, snd_pcm_uframes_t(frame_count)};
    auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_READI_FRAMES, &transfer);
    if (UNLIKELY(err < 0)) return {errno, 0};
    return {0, size_type(transfer.result)};
}

// ============================================================================
// Interleaved writer
// ============================================================================

result interleaved_pcm_writer::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_playback_device(card, device, non_blocking);
}

generic_result<size_type> interleaved_pcm_writer::write_unformatted(const void *frames, size_type frame_count) noexcept {
    snd_xferi transfer;
    transfer.result = 0;
    transfer.buf = const_cast<void *>(frames);
    transfer.frames = static_cast<snd_pcm_uframes_t>(frame_count);
    auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer);
    if (UNLIKELY(err < 0)) return {errno, 0};
    return {0, static_cast<size_type>(transfer.result)};
}

// ============================================================================
// Non-interleaved reader
// ============================================================================

result noninterleaved_pcm_reader::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_capture_device(card, device, non_blocking);
}

generic_result<size_type> noninterleaved_pcm_reader::read_unformatted(void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept {
    if (UNLIKELY(channel_count != channels_)) return {EINVAL, 0};

    snd_xfern transfer;
    transfer.result = 0;
    transfer.bufs = const_cast<void **>(channel_buffers);
    transfer.frames = static_cast<snd_pcm_uframes_t>(frame_count);
    auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_READN_FRAMES, &transfer);
    if (UNLIKELY(err < 0)) return {errno, 0};
    return {0, static_cast<size_type>(transfer.result)};
}

// ============================================================================
// Non-interleaved writer
// ============================================================================

result noninterleaved_pcm_writer::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_playback_device(card, device, non_blocking);
}

generic_result<size_type> noninterleaved_pcm_writer::write_unformatted(const void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept {
    if (UNLIKELY(channel_count != channels_)) return {EINVAL, 0};

    snd_xfern transfer;
    transfer.result = 0;
    transfer.bufs = const_cast<void **>(channel_buffers);
    transfer.frames = static_cast<snd_pcm_uframes_t>(frame_count);
    auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_WRITEN_FRAMES, &transfer);
    if (UNLIKELY(err < 0)) return {errno, 0};
    return {0, static_cast<size_type>(transfer.result)};
}

// ============================================================================
// pcm_impl
// ============================================================================

class pcm_impl final {
    friend class pcm;
    int fd = invalid_fd();
    bool is_capture = false;

    result open_by_path(const char *path, bool non_blocking) noexcept;
};

namespace {

pcm_impl *lazy_init(pcm_impl *impl) noexcept {
    if (impl) return impl;
    return new (std::nothrow) pcm_impl();
}

} // namespace

// ============================================================================
// pcm
// ============================================================================

pcm::pcm() noexcept
    : self(nullptr) {
}

pcm::pcm(pcm &&other) noexcept
    : self(other.self) {
    other.self = nullptr;
}

pcm::~pcm() {
    close();
    delete self;
}

int pcm::close() noexcept {
    if (!self) return 0;
    if (self->fd != invalid_fd()) {
        auto r = ::close(self->fd);
        self->fd = invalid_fd();
        if (r == -1) return errno;
    }
    return 0;
}

int pcm::get_file_descriptor() const noexcept {
    return self ? self->fd : invalid_fd();
}

bool pcm::is_open() const noexcept {
    return self && self->fd != invalid_fd();
}

generic_result<pcm_info> pcm::get_info() const noexcept {
    using result_type = generic_result<pcm_info>;
    if (!self) return result_type{ENOENT};
    snd_pcm_info native_info{};
    int err = ioctl(self->fd, SNDRV_PCM_IOCTL_INFO, &native_info);
    if (err != 0) return result_type{errno};
    return result_type{0, to_tinyalsa_info(native_info)};
}

result pcm::prepare() noexcept {
    if (!self) return ENOENT;
    auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_PREPARE);
    if (err < 0) return errno;
    return result();
}

result pcm::start() noexcept {
    if (!self) return ENOENT;
    auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_START);
    if (err < 0) return errno;
    return result();
}

result pcm::drop() noexcept {
    if (!self) return ENOENT;
    auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_DROP);
    if (err < 0) return errno;
    return result();
}

result pcm::drain() noexcept {
    if (!self) return ENOENT;
    auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_DRAIN);
    if (err < 0) return errno;
    return result();
}

result pcm::pause(bool enable) noexcept {
    if (!self) return ENOENT;
    int flag = enable ? 1 : 0;
    auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_PAUSE, flag);
    if (err < 0) return errno;
    return result();
}

generic_result<pcm_state> pcm::get_state() const noexcept {
    using R = generic_result<pcm_state>;
    if (!self) return R{ENOENT};
    snd_pcm_status status{};
    int err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_STATUS, &status);
    if (err < 0) return R{errno};
    return R{0, to_tinyalsa_state(status.state)};
}

generic_result<size_type> pcm::get_avail() const noexcept {
    using R = generic_result<size_type>;
    if (!self) return R{ENOENT};
    snd_pcm_status status{};
    int err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_STATUS, &status);
    if (err < 0) return R{errno};
    return R{0, static_cast<size_type>(status.avail)};
}

generic_result<long> pcm::get_delay() const noexcept {
    using R = generic_result<long>;
    if (!self) return R{ENOENT};
    snd_pcm_sframes_t delay = 0;
    int err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_DELAY, &delay);
    if (err < 0) return R{errno};
    return R{0, static_cast<long>(delay)};
}

int pcm::get_poll_events() const noexcept {
    if (!self || self->fd == invalid_fd()) return 0;
    return (self->is_capture ? POLLIN : POLLOUT) | POLLERR;
}

generic_result<size_type> pcm::rewind(size_type frames) noexcept {
    using R = generic_result<size_type>;
    if (!self) return R{ENOENT};
    auto n = static_cast<snd_pcm_uframes_t>(frames);
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_REWIND, &n) < 0) return R{errno};
    return R{0, static_cast<size_type>(n)};
}

generic_result<size_type> pcm::forward(size_type frames) noexcept {
    using R = generic_result<size_type>;
    if (!self) return R{ENOENT};
    auto n = static_cast<snd_pcm_uframes_t>(frames);
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_FORWARD, &n) < 0) return R{errno};
    return R{0, static_cast<size_type>(n)};
}

generic_result<pcm_channel_layout> pcm::get_channel_info(size_type channel) const noexcept {
    using R = generic_result<pcm_channel_layout>;
    if (!self) return R{ENOENT};
    snd_pcm_channel_info info{};
    info.channel = static_cast<unsigned int>(channel);
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_CHANNEL_INFO, &info) < 0) return R{errno};

    pcm_channel_layout layout{};
    layout.mmap_offset = static_cast<long>(info.offset);
    layout.first_bit = static_cast<size_type>(info.first);
    layout.step_bits = static_cast<size_type>(info.step);
    return R{0, layout};
}

generic_result<protocol_version> pcm::get_protocol_version() const noexcept {
    using R = generic_result<protocol_version>;
    if (!self) return R{ENOENT};
    int raw = 0;
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_PVERSION, &raw) < 0) return R{errno};

    protocol_version v;
    v.major = static_cast<unsigned int>(raw >> 16) & 0xffffu;
    v.minor = static_cast<unsigned int>(raw >> 8) & 0xffu;
    v.subminor = static_cast<unsigned int>(raw) & 0xffu;
    return R{0, v};
}

generic_result<pcm::timestamp> pcm::get_timestamp() const noexcept {
    using R = generic_result<timestamp>;
    if (!self) return R{ENOENT};
    snd_pcm_status status{};
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_STATUS, &status) < 0) return R{errno};

    timestamp ts;
    ts.seconds = static_cast<long>(status.tstamp.tv_sec);
    ts.nanoseconds = static_cast<long>(status.tstamp.tv_nsec);
    return R{0, ts};
}

namespace {

// Bit layout fixed by snd_pcm_(un)pack_audio_tstamp_config/_report in the kernel's
// <sound/pcm.h>: low 16 bits of audio_tstamp_data are the request (type in bits
// 0-3, report_delay in bit 4); the high 16 bits are the driver's report (valid in
// bit 16, actual_type in bits 17-20, accuracy_report in bit 21) after the ioctl.
constexpr unsigned int pack_audio_tstamp_request(pcm::audio_tstamp_type requested) noexcept {
    return static_cast<unsigned int>(requested) & 0xFu;
}

} // namespace

generic_result<pcm::extended_timestamp> pcm::get_extended_timestamp(audio_tstamp_type requested_type) const noexcept {
    using R = generic_result<extended_timestamp>;
    if (!self) return R{ENOENT};

    snd_pcm_status status{};
    status.audio_tstamp_data = pack_audio_tstamp_request(requested_type);
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_STATUS_EXT, &status) < 0) return R{errno};

    extended_timestamp out{};
    out.system.seconds = static_cast<long>(status.tstamp.tv_sec);
    out.system.nanoseconds = static_cast<long>(status.tstamp.tv_nsec);
    out.audio.seconds = static_cast<long>(status.audio_tstamp.tv_sec);
    out.audio.nanoseconds = static_cast<long>(status.audio_tstamp.tv_nsec);

    unsigned int report = status.audio_tstamp_data >> 16;
    bool valid = (report & 0x1u) != 0;
    out.actual_type = static_cast<audio_tstamp_type>((report >> 1) & 0xFu);
    out.accuracy_valid = valid && ((report >> 5) & 0x1u) != 0;
    out.accuracy_ns = out.accuracy_valid ? status.audio_tstamp_accuracy : 0;
    return R{0, out};
}

result pcm::setup(const pcm_config &config, sample_access access, bool is_capture) noexcept {
    auto hw_params = to_alsa_hw_params(config, access);
    auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_HW_PARAMS, &hw_params);
    if (err < 0) return errno;

    auto sw_params = to_alsa_sw_params(config, is_capture);
    err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_SW_PARAMS, &sw_params);
    if (err < 0) return errno;

    return 0;
}

result pcm::link(const pcm &other) noexcept {
    if (!self) return ENOENT;
    int other_fd = other.get_file_descriptor();
    if (other_fd == invalid_fd()) return EINVAL;
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_LINK, other_fd) < 0) return errno;
    return result();
}

result pcm::unlink() noexcept {
    if (!self) return ENOENT;
    if (::ioctl(self->fd, SNDRV_PCM_IOCTL_UNLINK) < 0) return errno;
    return result();
}

result pcm::open_capture_device(size_type card, size_type device, bool non_blocking) noexcept {
    self = lazy_init(self);
    if (!self) return result{ENOMEM};
    self->is_capture = true;
    char path[256];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%luD%luc", (unsigned long)card, (unsigned long)device);
    return self->open_by_path(path, non_blocking);
}

result pcm::open_playback_device(size_type card, size_type device, bool non_blocking) noexcept {
    self = lazy_init(self);
    if (!self) return result{ENOMEM};
    self->is_capture = false;
    char path[256];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%luD%lup", (unsigned long)card, (unsigned long)device);
    return self->open_by_path(path, non_blocking);
}

result pcm_impl::open_by_path(const char *path, bool non_blocking) noexcept {
    if (fd != invalid_fd()) ::close(fd);
    fd = ::open(path, non_blocking ? (O_RDWR | O_NONBLOCK) : O_RDWR);
    if (fd < 0) {
        fd = invalid_fd();
        return result{errno};
    }
    return result{0};
}

// ============================================================================
// mmap helpers
// ============================================================================

namespace {

/**
 * @brief Result of negotiating hw/sw params and mapping the DMA buffer.
 *
 * status_page/control_page are non-null only when the driver allows the
 * lock-free direct path (see needs_explicit_sync below); callers must fall
 * back to SNDRV_PCM_IOCTL_SYNC_PTR when they are null.
 */
struct mmap_setup_result final {
    void *data_ptr = nullptr;
    void *status_page = nullptr;
    void *control_page = nullptr;
    size_type buffer_frames = 0;
    size_type frame_bytes = 0;
    size_type boundary = 0;
};

/**
 * Maps the read-only status page and read-write control page so hw_ptr/appl_ptr
 * can be exchanged with the kernel by plain memory access instead of an ioctl
 * per period. Only safe when the driver has not set SNDRV_PCM_INFO_SYNC_APPLPTR
 * or SNDRV_PCM_INFO_EXPLICIT_SYNC in the info field HW_PARAMS returned; those
 * flags mean the driver does not keep the mmap'd pages coherent on its own and
 * requires the explicit SYNC_PTR ioctl instead. Returns false (data pages left
 * unmapped) rather than treating a mmap failure on these optional pages as fatal,
 * since the ioctl-based path in mmap_begin_write/mmap_begin_read/mmap_commit_common
 * remains fully correct on any driver.
 */
bool try_map_direct_pointers(int fd, unsigned int hw_params_info, void **out_status, void **out_control) noexcept {
    constexpr unsigned int explicit_sync_flags = SNDRV_PCM_INFO_SYNC_APPLPTR | SNDRV_PCM_INFO_EXPLICIT_SYNC;
    if (hw_params_info & explicit_sync_flags) return false;

    void *status = ::mmap(nullptr, sizeof(snd_pcm_mmap_status), PROT_READ, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_STATUS);
    if (status == MAP_FAILED) return false;

    void *control = ::mmap(nullptr, sizeof(snd_pcm_mmap_control), PROT_READ | PROT_WRITE, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_CONTROL);
    if (control == MAP_FAILED) {
        ::munmap(status, sizeof(snd_pcm_mmap_status));
        return false;
    }

    *out_status = status;
    *out_control = control;
    return true;
}

int mmap_setup_common(int fd, const pcm_config &config, sample_access access, bool is_capture, mmap_setup_result *out) noexcept {
    auto hw_params = to_alsa_hw_params(config, access);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw_params) < 0) return errno;

    auto sw_params = to_alsa_sw_params(config, is_capture);
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw_params) < 0) return errno;

    // Derive sizes from the requested (exact) config.
    // Because we set integer=1 with min==max, the negotiated values equal
    // what we requested, or HW_PARAMS would have failed.
    size_type buffer_frames = config.period_size * config.period_count;
    size_type frame_bytes = bytes_per_frame(config.format, config.channels);
    if (frame_bytes == 0) return EINVAL;

    size_type map_size = buffer_frames * frame_bytes;
    void *ptr = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_DATA);
    if (ptr == MAP_FAILED) return errno;

    out->data_ptr = ptr;
    out->buffer_frames = buffer_frames;
    out->frame_bytes = frame_bytes;
    out->boundary = compute_boundary(buffer_frames);

    void *status = nullptr;
    void *control = nullptr;
    if (try_map_direct_pointers(fd, hw_params.info, &status, &control)) {
        out->status_page = status;
        out->control_page = control;
    }
    return 0;
}

void mmap_teardown_common(void *ptr, void *status_page, void *control_page, size_type buffer_frames, size_type frame_bytes) noexcept {
    if (ptr && ptr != MAP_FAILED) ::munmap(ptr, buffer_frames * frame_bytes);
    if (status_page) ::munmap(status_page, sizeof(snd_pcm_mmap_status));
    if (control_page) ::munmap(control_page, sizeof(snd_pcm_mmap_control));
}

/**
 * Given a fresh hw_ptr, computes the writable slice of the ring buffer.
 * Shared by both the direct-mmap and SYNC_PTR-ioctl paths so the xrun/wrap
 * math exists in exactly one place.
 */
int compute_write_region(
    size_type hw_ptr, size_type appl_ptr, size_type buffer_frames, size_type boundary, size_type frame_bytes, void *mmap_data, mmap_region *out
) noexcept {
    size_type used = (appl_ptr >= hw_ptr) ? (appl_ptr - hw_ptr) : (boundary - (hw_ptr - appl_ptr));
    if (UNLIKELY(used > buffer_frames)) return EPIPE; // underrun

    size_type avail = buffer_frames - used;
    if (UNLIKELY(avail == 0)) return EAGAIN;

    size_type offset = appl_ptr % buffer_frames;
    size_type contig = buffer_frames - offset;
    if (avail > contig) avail = contig;

    out->data = static_cast<char *>(mmap_data) + offset * frame_bytes;
    out->offset = offset;
    out->avail = avail;
    return 0;
}

/** @brief Given a fresh hw_ptr, computes the readable slice of the ring buffer. */
int compute_read_region(
    size_type hw_ptr, size_type appl_ptr, size_type buffer_frames, size_type boundary, size_type frame_bytes, void *mmap_data, mmap_region *out
) noexcept {
    size_type avail = (hw_ptr >= appl_ptr) ? (hw_ptr - appl_ptr) : (boundary - (appl_ptr - hw_ptr));
    if (UNLIKELY(avail > buffer_frames)) return EPIPE; // overrun
    if (UNLIKELY(avail == 0)) return EAGAIN;

    size_type offset = appl_ptr % buffer_frames;
    size_type contig = buffer_frames - offset;
    if (avail > contig) avail = contig;

    out->data = static_cast<char *>(mmap_data) + offset * frame_bytes;
    out->offset = offset;
    out->avail = avail;
    return 0;
}

/** @brief Reads hw_ptr straight out of the mapped status page with an acquire load. */
size_type read_hw_ptr_direct(const void *status_page) noexcept {
    const auto *status = static_cast<const snd_pcm_mmap_status *>(status_page);
    return static_cast<size_type>(__atomic_load_n(&status->hw_ptr, __ATOMIC_ACQUIRE));
}

/** @brief Publishes appl_ptr into the mapped control page with a release store. */
void write_appl_ptr_direct(void *control_page, size_type value) noexcept {
    auto *control = static_cast<snd_pcm_mmap_control *>(control_page);
    __atomic_store_n(&control->appl_ptr, static_cast<snd_pcm_uframes_t>(value), __ATOMIC_RELEASE);
}

int mmap_begin_write(
    int fd, size_type appl_ptr, size_type buffer_frames, size_type boundary, size_type frame_bytes, void *mmap_data, mmap_region *out
) noexcept {
    snd_pcm_sync_ptr sptr{};
    sptr.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL;
    if (UNLIKELY(ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0)) return errno;
    return compute_write_region(sptr.s.status.hw_ptr, appl_ptr, buffer_frames, boundary, frame_bytes, mmap_data, out);
}

int mmap_begin_read(
    int fd, size_type appl_ptr, size_type buffer_frames, size_type boundary, size_type frame_bytes, void *mmap_data, mmap_region *out
) noexcept {
    snd_pcm_sync_ptr sptr{};
    sptr.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL;
    if (UNLIKELY(ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0)) return errno;
    return compute_read_region(sptr.s.status.hw_ptr, appl_ptr, buffer_frames, boundary, frame_bytes, mmap_data, out);
}

int mmap_commit_common(int fd, size_type *appl_ptr, size_type frames, size_type boundary, size_type avail_min) noexcept {
    *appl_ptr += frames;
    if (*appl_ptr >= boundary) *appl_ptr -= boundary;

    snd_pcm_sync_ptr sptr{};
    sptr.flags = 0;
    sptr.c.control.appl_ptr = *appl_ptr;
    sptr.c.control.avail_min = avail_min;
    if (UNLIKELY(ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0)) return errno;
    return 0;
}

} // namespace

// ============================================================================
// mmap_pcm_writer
// ============================================================================

mmap_pcm_writer::~mmap_pcm_writer() {
    mmap_teardown_common(mmap_data_, status_page_, control_page_, buffer_frames_, frame_bytes_);
}

result mmap_pcm_writer::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_playback_device(card, device, non_blocking);
}

result mmap_pcm_writer::setup(const pcm_config &config) noexcept {
    mmap_teardown_common(mmap_data_, status_page_, control_page_, buffer_frames_, frame_bytes_);
    mmap_data_ = nullptr;
    status_page_ = nullptr;
    control_page_ = nullptr;
    buffer_frames_ = 0;
    frame_bytes_ = 0;
    appl_ptr_ = 0;
    boundary_ = 0;
    avail_min_ = 0;
    has_direct_pointers_ = false;

    mmap_setup_result setup_result{};
    int err = mmap_setup_common(get_file_descriptor(), config, sample_access::mmap_interleaved, false, &setup_result);
    if (err) return err;

    mmap_data_ = setup_result.data_ptr;
    status_page_ = setup_result.status_page;
    control_page_ = setup_result.control_page;
    buffer_frames_ = setup_result.buffer_frames;
    frame_bytes_ = setup_result.frame_bytes;
    boundary_ = setup_result.boundary;
    has_direct_pointers_ = status_page_ != nullptr;
    avail_min_ = config.period_size;
    return 0;
}

generic_result<mmap_region> mmap_pcm_writer::begin() noexcept {
    using R = generic_result<mmap_region>;
    mmap_region rgn{};
    int err;
    if (has_direct_pointers_) {
        size_type hw_ptr = read_hw_ptr_direct(status_page_);
        err = compute_write_region(hw_ptr, appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    } else {
        err = mmap_begin_write(get_file_descriptor(), appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    }
    if (err) return R{err};
    return R{0, rgn};
}

result mmap_pcm_writer::commit(size_type frames) noexcept {
    if (has_direct_pointers_) {
        appl_ptr_ += frames;
        if (appl_ptr_ >= boundary_) appl_ptr_ -= boundary_;
        write_appl_ptr_direct(control_page_, appl_ptr_);
        return 0;
    }
    return mmap_commit_common(get_file_descriptor(), &appl_ptr_, frames, boundary_, avail_min_);
}

// ============================================================================
// mmap_pcm_reader
// ============================================================================

mmap_pcm_reader::~mmap_pcm_reader() {
    mmap_teardown_common(mmap_data_, status_page_, control_page_, buffer_frames_, frame_bytes_);
}

result mmap_pcm_reader::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_capture_device(card, device, non_blocking);
}

result mmap_pcm_reader::setup(const pcm_config &config) noexcept {
    mmap_teardown_common(mmap_data_, status_page_, control_page_, buffer_frames_, frame_bytes_);
    mmap_data_ = nullptr;
    status_page_ = nullptr;
    control_page_ = nullptr;
    buffer_frames_ = 0;
    frame_bytes_ = 0;
    appl_ptr_ = 0;
    boundary_ = 0;
    avail_min_ = 0;
    has_direct_pointers_ = false;

    mmap_setup_result setup_result{};
    int err = mmap_setup_common(get_file_descriptor(), config, sample_access::mmap_interleaved, true, &setup_result);
    if (err) return err;

    mmap_data_ = setup_result.data_ptr;
    status_page_ = setup_result.status_page;
    control_page_ = setup_result.control_page;
    buffer_frames_ = setup_result.buffer_frames;
    frame_bytes_ = setup_result.frame_bytes;
    boundary_ = setup_result.boundary;
    has_direct_pointers_ = status_page_ != nullptr;
    avail_min_ = config.period_size;
    return 0;
}

generic_result<mmap_region> mmap_pcm_reader::begin() noexcept {
    using R = generic_result<mmap_region>;
    mmap_region rgn{};
    int err;
    if (has_direct_pointers_) {
        size_type hw_ptr = read_hw_ptr_direct(status_page_);
        err = compute_read_region(hw_ptr, appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    } else {
        err = mmap_begin_read(get_file_descriptor(), appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    }
    if (err) return R{err};
    return R{0, rgn};
}

result mmap_pcm_reader::commit(size_type frames) noexcept {
    if (has_direct_pointers_) {
        appl_ptr_ += frames;
        if (appl_ptr_ >= boundary_) appl_ptr_ -= boundary_;
        write_appl_ptr_direct(control_page_, appl_ptr_);
        return 0;
    }
    return mmap_commit_common(get_file_descriptor(), &appl_ptr_, frames, boundary_, avail_min_);
}

// ============================================================================
// pcm_period_timer
// ============================================================================

pcm_period_timer::pcm_period_timer(pcm_period_timer &&other) noexcept
    : fd(other.fd) {
    other.fd = invalid_fd();
}

pcm_period_timer &pcm_period_timer::operator=(pcm_period_timer &&other) noexcept {
    if (this != &other) {
        close();
        fd = other.fd;
        other.fd = invalid_fd();
    }
    return *this;
}

pcm_period_timer::~pcm_period_timer() {
    close();
}

result pcm_period_timer::open(size_type card, size_type device, bool is_capture, size_type subdevice) noexcept {
    close();

    fd = ::open("/dev/snd/timer", O_RDWR);
    if (fd < 0) {
        fd = invalid_fd();
        return result{errno};
    }

    snd_timer_select select{};
    select.id.dev_class = SNDRV_TIMER_CLASS_PCM;
    select.id.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
    select.id.card = static_cast<int>(card);
    select.id.device = static_cast<int>(device);
    select.id.subdevice = static_cast<int>((subdevice << 1) | (is_capture ? 1u : 0u));
    if (::ioctl(fd, SNDRV_TIMER_IOCTL_SELECT, &select) < 0) {
        int err = errno;
        ::close(fd);
        fd = invalid_fd();
        return result{err};
    }

    snd_timer_params params{};
    params.flags = SNDRV_TIMER_PSFLG_AUTO;
    params.ticks = 1;
    params.queue_size = 32;
    if (::ioctl(fd, SNDRV_TIMER_IOCTL_PARAMS, &params) < 0) {
        int err = errno;
        ::close(fd);
        fd = invalid_fd();
        return result{err};
    }

    if (::ioctl(fd, SNDRV_TIMER_IOCTL_START) < 0) {
        int err = errno;
        ::close(fd);
        fd = invalid_fd();
        return result{err};
    }
    return result();
}

void pcm_period_timer::close() noexcept {
    if (fd != invalid_fd()) {
        ::close(fd);
        fd = invalid_fd();
    }
}

bool pcm_period_timer::is_open() const noexcept {
    return fd != invalid_fd();
}

int pcm_period_timer::get_file_descriptor() const noexcept {
    return fd;
}

generic_result<size_type> pcm_period_timer::wait_for_tick() noexcept {
    using R = generic_result<size_type>;
    if (fd == invalid_fd()) return R{ENOENT};

    snd_timer_read tick{};
    auto n = ::read(fd, &tick, sizeof(tick));
    if (n < 0) return R{errno};
    if (static_cast<size_type>(n) != sizeof(tick)) return R{EIO};
    return R{0, static_cast<size_type>(tick.ticks)};
}

// ============================================================================
// pcm_list
// ============================================================================

namespace {

struct dir_wrapper final {
    DIR *ptr = nullptr;
    inline operator DIR *() noexcept {
        return ptr;
    }
    ~dir_wrapper() {
        if (ptr) closedir(ptr);
    }
    bool open(const char *path) noexcept {
        ptr = opendir(path);
        return !!ptr;
    }
};

struct parsed_name final {
    bool valid = false;
    size_type card = 0;
    size_type device = 0;
    bool is_capture = false;

    parsed_name(const char *name) noexcept {
        valid = parse(name);
    }

private:
    bool parse(const char *name) noexcept;

    static constexpr bool is_dec(char c) noexcept {
        return (c >= '0') && (c <= '9');
    }
    constexpr size_type to_dec(char c) noexcept {
        return size_type(c - '0');
    }
};

bool parsed_name::parse(const char *name) noexcept {
    auto name_length = strlen(name);
    if (!name_length) return false;
    if ((name[0] != 'p') || (name[1] != 'c') || (name[2] != 'm') || (name[3] != 'C')) return false;

    size_type d_pos = name_length;
    for (size_type i = 4; i < name_length; i++) {
        if (name[i] == 'D') {
            d_pos = i;
            break;
        }
    }
    if (d_pos >= name_length) return false;

    if (name[name_length - 1] == 'c') is_capture = true;
    else if (name[name_length - 1] == 'p')
        is_capture = false;
    else
        return false;

    device = 0;
    card = 0;
    for (size_type i = 4; i < d_pos; i++) {
        if (!is_dec(name[i])) return false;
        card = card * 10 + to_dec(name[i]);
    }
    for (size_type i = d_pos + 1; i < (name_length - 1); i++) {
        if (!is_dec(name[i])) return false;
        device = device * 10 + to_dec(name[i]);
    }
    return true;
}

} // namespace

class pcm_list_impl final {
    friend class pcm_list;
    pod_buffer<pcm_info> info_buffer;
};

pcm_list::pcm_list() noexcept
    : self(nullptr) {
    self = new (std::nothrow) pcm_list_impl();
    if (!self) return;

    dir_wrapper snd_dir;
    if (!snd_dir.open("/dev/snd")) return;

    dirent *entry = nullptr;
    for (;;) {
        entry = readdir(snd_dir);
        if (!entry) break;
        parsed_name name(entry->d_name);
        if (!name.valid) continue;

        pcm p;
        result open_result;
        if (name.is_capture) open_result = p.open_capture_device(name.card, name.device);
        else
            open_result = p.open_playback_device(name.card, name.device);
        if (open_result.failed()) continue;

        auto info_result = p.get_info();
        if (info_result.failed()) continue;

        auto info = info_result.unwrap();
        info.is_capture = name.is_capture;
        if (!self->info_buffer.emplace_back(std::move(info))) break;
    }
}

pcm_list::pcm_list(pcm_list &&other) noexcept
    : self(other.self) {
    other.self = nullptr;
}
pcm_list::~pcm_list() {
    delete self;
}

const pcm_info *pcm_list::data() const noexcept {
    return self ? self->info_buffer.data : nullptr;
}
size_type pcm_list::size() const noexcept {
    return self ? self->info_buffer.size : 0;
}

// ============================================================================
// pcm_params
// ============================================================================

class pcm_params_impl {
    friend class pcm_params;
    int fd = invalid_fd();
    snd_pcm_hw_params params{};
};

pcm_params::pcm_params() noexcept
    : self(nullptr) {
}

pcm_params::pcm_params(pcm_params &&other) noexcept
    : self(other.self) {
    other.self = nullptr;
}

pcm_params &pcm_params::operator=(pcm_params &&other) noexcept {
    if (this != &other) {
        close();
        self = other.self;
        other.self = nullptr;
    }
    return *this;
}

pcm_params::~pcm_params() {
    close();
}

result pcm_params::open(size_type card, size_type device, bool is_capture) noexcept {
    close();
    self = new (std::nothrow) pcm_params_impl();
    if (!self) return {ENOMEM};

    char path[256];
    snprintf(
        path,
        sizeof(path),
        is_capture ? "/dev/snd/pcmC%luD%luc" : "/dev/snd/pcmC%luD%lup",
        (unsigned long)card,
        (unsigned long)device
    );

    self->fd = ::open(path, O_RDWR | O_NONBLOCK);
    if (self->fd < 0) {
        delete self;
        self = nullptr;
        return {errno};
    }

    self->params = init_hw_parameters();
    int err = ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &self->params);
    if (err < 0) {
        ::close(self->fd);
        delete self;
        self = nullptr;
        return {errno};
    }
    return {0};
}

void pcm_params::close() noexcept {
    if (self) {
        if (self->fd != invalid_fd()) ::close(self->fd);
        delete self;
        self = nullptr;
    }
}

bool pcm_params::is_open() const noexcept {
    return self && self->fd != invalid_fd();
}

bool pcm_params::test_format(sample_format fmt) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    mask_ref<SNDRV_PCM_HW_PARAM_FORMAT>::set(test, to_alsa_format(fmt));
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_rate(size_type rate) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_RATE>::set(test, rate);
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_channels(size_type ch) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_CHANNELS>::set(test, ch);
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_config(size_type channels, size_type rate, sample_format fmt) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_CHANNELS>::set(test, channels);
    interval_ref<SNDRV_PCM_HW_PARAM_RATE>::set(test, rate);
    mask_ref<SNDRV_PCM_HW_PARAM_FORMAT>::set(test, to_alsa_format(fmt));
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_period_size(size_type ps) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_PERIOD_SIZE>::set(test, ps);
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_period_count(size_type pc) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_PERIODS>::set(test, pc);
    test.rmask = ~0U;
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

void pcm_params::for_each_supported_format(void (*callback)(sample_format, void *), void *user_data) const noexcept {
    if (!is_open() || !callback) return;

    static const sample_format all_formats[] = {
        sample_format::s8,      sample_format::s16_le,  sample_format::s16_be,  sample_format::s18_3le, sample_format::s18_3be,
        sample_format::s20_3le, sample_format::s20_3be, sample_format::s24_3le, sample_format::s24_3be, sample_format::s24_le,
        sample_format::s24_be,  sample_format::s32_le,  sample_format::s32_be,  sample_format::u8,      sample_format::u16_le,
        sample_format::u16_be,  sample_format::u18_3le, sample_format::u18_3be, sample_format::u20_3le, sample_format::u20_3be,
        sample_format::u24_3le, sample_format::u24_3be, sample_format::u24_le,  sample_format::u24_be,  sample_format::u32_le,
        sample_format::u32_be,  sample_format::dsd_u8,  sample_format::dsd_u16_le, sample_format::dsd_u16_be, sample_format::dsd_u32_le,
        sample_format::dsd_u32_be,
    };

    for (auto fmt : all_formats) {
        // Use the already-refined params as a starting mask and pin only FORMAT.
        auto test = self->params;
        mask_ref<SNDRV_PCM_HW_PARAM_FORMAT>::set(test, to_alsa_format(fmt));
        test.rmask = ~0U;
        if (ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0) callback(fmt, user_data);
    }
}

generic_result<size_type> pcm_params::get_min_rate() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.min};
}

generic_result<size_type> pcm_params::get_max_rate() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.max};
}

generic_result<size_type> pcm_params::get_min_channels() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.min};
}

generic_result<size_type> pcm_params::get_max_channels() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.max};
}

generic_result<size_type> pcm_params::get_min_period_size() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.min};
}

generic_result<size_type> pcm_params::get_max_period_size() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.max};
}

generic_result<size_type> pcm_params::get_min_period_count() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.min};
}

generic_result<size_type> pcm_params::get_max_period_count() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.max};
}

generic_result<size_type> pcm_params::get_min_buffer_size() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.min};
}

unsigned int pcm_params::get_capabilities() const noexcept {
    return is_open() ? self->params.info : 0;
}

bool pcm_params::supports_pause() const noexcept {
    return (get_capabilities() & SNDRV_PCM_INFO_PAUSE) != 0;
}

bool pcm_params::supports_resume() const noexcept {
    return (get_capabilities() & SNDRV_PCM_INFO_RESUME) != 0;
}

bool pcm_params::supports_mmap() const noexcept {
    return (get_capabilities() & SNDRV_PCM_INFO_MMAP) != 0;
}

bool pcm_params::needs_explicit_sync() const noexcept {
    return (get_capabilities() & (SNDRV_PCM_INFO_SYNC_APPLPTR | SNDRV_PCM_INFO_EXPLICIT_SYNC)) != 0;
}

generic_result<size_type> pcm_params::get_max_buffer_size() const noexcept {
    if (!is_open()) return {ENOENT};
    auto &i = self->params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    return {0, i.max};
}

// ============================================================================
// pcm_recover
// ============================================================================

result pcm_recover(pcm &p, int err, bool silent) noexcept {
    (void)silent;

    // EAGAIN means "no frames available right now" in non-blocking mode.
    // The stream is not broken, so running PREPARE here would needlessly
    // drop buffered audio; just report it back to the caller to retry.
    if (err == EAGAIN) return {EAGAIN};

    if (err == EPIPE) return p.prepare();

    if (err == ESTRPIPE) {
        // Kernel-suspended device (e.g. system sleep). Per the ALSA
        // suspend/resume protocol, try RESUME first so buffered audio and
        // stream position survive; only fall back to PREPARE (which drops
        // buffered audio) if the driver has no resume support.
        int fd = p.get_file_descriptor();
        int r;
        for (;;) {
            r = ::ioctl(fd, SNDRV_PCM_IOCTL_RESUME);
            if (r >= 0) return {0};
            if (errno != EAGAIN) break;
            usleep(1000);
        }
        if (errno == ENOSYS || errno == EINVAL) return p.prepare();
        return {errno};
    }

    return {err};
}

// ============================================================================
// mixer_ctl
// ============================================================================

// Stride of each enum item name in the flat enum_names_ buffer.
static constexpr size_t kEnumNameLen = sizeof(snd_ctl_elem_info{}.value.enumerated.name);

mixer_ctl::mixer_ctl(mixer_ctl &&other) noexcept
    : fd(other.fd)
    , min_(other.min_)
    , max_(other.max_)
    , min64_(other.min64_)
    , max64_(other.max64_)
    , count_(other.count_)
    , elem_type_(other.elem_type_)
    , numid_(other.numid_)
    , iface_(other.iface_)
    , device_(other.device_)
    , subdevice_(other.subdevice_)
    , index_(other.index_)
    , enum_names_(other.enum_names_)
    , enum_items_count_(other.enum_items_count_)
    , has_tlv_(other.has_tlv_) {
    memcpy(name_, other.name_, sizeof(name_));
    other.fd = invalid_fd();
    other.enum_names_ = nullptr;
    other.enum_items_count_ = 0;
}

mixer_ctl &mixer_ctl::operator=(mixer_ctl &&other) noexcept {
    if (this != &other) {
        fd = other.fd;
        min_ = other.min_;
        max_ = other.max_;
        min64_ = other.min64_;
        max64_ = other.max64_;
        count_ = other.count_;
        elem_type_ = other.elem_type_;
        numid_ = other.numid_;
        iface_ = other.iface_;
        device_ = other.device_;
        subdevice_ = other.subdevice_;
        index_ = other.index_;
        enum_names_ = other.enum_names_;
        enum_items_count_ = other.enum_items_count_;
        has_tlv_ = other.has_tlv_;
        memcpy(name_, other.name_, sizeof(name_));
        other.fd = invalid_fd();
        other.enum_names_ = nullptr;
        other.enum_items_count_ = 0;
    }
    return *this;
}

// Type predicates

bool mixer_ctl::is_volume() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_INTEGER && count_ > 0 && min_ != max_;
}
bool mixer_ctl::is_integer() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_INTEGER;
}
bool mixer_ctl::is_boolean() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_BOOLEAN;
}
bool mixer_ctl::is_enum() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_ENUMERATED;
}
bool mixer_ctl::is_bytes() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_BYTES;
}
bool mixer_ctl::is_integer64() const noexcept {
    return elem_type_ == SNDRV_CTL_ELEM_TYPE_INTEGER64;
}

// Integer accessors

generic_result<long> mixer_ctl::get_value(size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    return {0, ev.value.integer.value[index]};
}

result mixer_ctl::set_value(long value, size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER) return {EINVAL};
    if (value < min_ || value > max_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    // Read current to preserve other element values (e.g. the other stereo channel)
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    ev.value.integer.value[index] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

result mixer_ctl::set_all_values(long value) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER) return {EINVAL};
    if (value < min_ || value > max_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    for (size_type i = 0; i < count_; ++i) ev.value.integer.value[i] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

// 64-bit integer accessors

generic_result<long long> mixer_ctl::get_int64(size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER64) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    return {0, ev.value.integer64.value[index]};
}

result mixer_ctl::set_int64(long long value, size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER64) return {EINVAL};
    if (value < min64_ || value > max64_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    // Read current to preserve other element values (e.g. the other stereo channel)
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    ev.value.integer64.value[index] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

result mixer_ctl::set_all_int64(long long value) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_INTEGER64) return {EINVAL};
    if (value < min64_ || value > max64_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    for (size_type i = 0; i < count_; ++i) ev.value.integer64.value[i] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

// Boolean accessors

generic_result<bool> mixer_ctl::get_bool(size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_BOOLEAN) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    return {0, ev.value.integer.value[index] != 0};
}

result mixer_ctl::set_bool(bool value, size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_BOOLEAN) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    ev.value.integer.value[index] = value ? 1 : 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

result mixer_ctl::set_all_bools(bool value) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_BOOLEAN) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    for (size_type i = 0; i < count_; ++i) ev.value.integer.value[i] = value ? 1 : 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

// Enumerated accessors

const char *mixer_ctl::get_enum_item_name(unsigned int item) const noexcept {
    if (!enum_names_ || item >= enum_items_count_) return nullptr;
    return enum_names_ + item * kEnumNameLen;
}

generic_result<size_type> mixer_ctl::get_enum_index(size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_ENUMERATED) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    return {0, static_cast<size_type>(ev.value.enumerated.item[index])};
}

result mixer_ctl::set_enum_index(unsigned int item, size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_ENUMERATED) return {EINVAL};
    if (item >= enum_items_count_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    ev.value.enumerated.item[index] = item;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

result mixer_ctl::set_enum_by_name(const char *name, size_type index) const noexcept {
    if (!enum_names_ || !name) return {EINVAL};

    for (unsigned int i = 0; i < enum_items_count_; ++i) {
        if (strcmp(enum_names_ + i * kEnumNameLen, name) == 0) return set_enum_index(i, index);
    }
    return {EINVAL};
}

result mixer_ctl::set_all_enum_indices(unsigned int item) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_ENUMERATED) return {EINVAL};
    if (item >= enum_items_count_) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    for (size_type i = 0; i < count_; ++i) ev.value.enumerated.item[i] = item;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

// Bytes accessors

generic_result<unsigned char> mixer_ctl::get_byte(size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_BYTES) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    return {0, ev.value.bytes.data[index]};
}

result mixer_ctl::set_byte(unsigned char value, size_type index) const noexcept {
    if (fd == invalid_fd()) return {ENOENT};
    if (index >= count_) return {EINVAL};
    if (elem_type_ != SNDRV_CTL_ELEM_TYPE_BYTES) return {EINVAL};

    snd_ctl_elem_value ev{};
    ev.id.numid = numid_;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0) return {errno};
    ev.value.bytes.data[index] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) < 0) return {errno};
    return {0};
}

// TLV / dB range accessors

generic_result<mixer_ctl::dB_range> mixer_ctl::get_dB_range() const noexcept {
    using R = generic_result<dB_range>;
    if (fd == invalid_fd()) return R{ENOENT};
    if (!has_tlv_) return R{ENOSYS};

    // 4 payload words covers every dB TLV shape this function decodes
    // (DB_SCALE and DB_MINMAX(_MUTE) are both {type, length, value0, value1}).
    unsigned char raw[sizeof(snd_ctl_tlv) + 4 * sizeof(unsigned int)];
    auto *tlv = reinterpret_cast<snd_ctl_tlv *>(raw);
    tlv->numid = numid_;
    tlv->length = 4 * sizeof(unsigned int);
    if (ioctl(fd, SNDRV_CTL_IOCTL_TLV_READ, tlv) < 0) return R{errno};
    if (tlv->length < 2 * sizeof(unsigned int)) return R{ENOTSUP};

    dB_range out{};
    switch (tlv->tlv[0]) {
        case SNDRV_CTL_TLVT_DB_SCALE:
            out.min_millibel = static_cast<long>(static_cast<int>(tlv->tlv[2]));
            out.step_millibel = static_cast<long>(tlv->tlv[3] & SNDRV_CTL_TLVD_DB_SCALE_MASK);
            out.has_mute = (tlv->tlv[3] & SNDRV_CTL_TLVD_DB_SCALE_MUTE) != 0;
            out.max_millibel = out.min_millibel + out.step_millibel * (max_ - min_);
            return R{0, out};

        case SNDRV_CTL_TLVT_DB_MINMAX:
        case SNDRV_CTL_TLVT_DB_MINMAX_MUTE:
            out.min_millibel = static_cast<long>(static_cast<int>(tlv->tlv[2]));
            out.max_millibel = static_cast<long>(static_cast<int>(tlv->tlv[3]));
            out.has_mute = (tlv->tlv[0] == SNDRV_CTL_TLVT_DB_MINMAX_MUTE);
            return R{0, out};

        default:
            // DB_RANGE (piecewise) and DB_LINEAR shapes need the full payload
            // decoded by the caller; get_tlv_raw() exposes it unmodified.
            return R{ENOTSUP};
    }
}

generic_result<size_type> mixer_ctl::get_tlv_raw(unsigned int *buffer, size_type buffer_words) const noexcept {
    using R = generic_result<size_type>;
    if (fd == invalid_fd()) return R{ENOENT};
    if (!has_tlv_) return R{ENOSYS};
    if (!buffer || buffer_words == 0) return R{EINVAL};

    size_type alloc_bytes = sizeof(snd_ctl_tlv) + buffer_words * sizeof(unsigned int);
    auto *raw = static_cast<unsigned char *>(malloc(alloc_bytes));
    if (!raw) return R{ENOMEM};

    auto *tlv = reinterpret_cast<snd_ctl_tlv *>(raw);
    tlv->numid = numid_;
    tlv->length = static_cast<unsigned int>(buffer_words * sizeof(unsigned int));
    if (ioctl(fd, SNDRV_CTL_IOCTL_TLV_READ, tlv) < 0) {
        int err = errno;
        free(raw);
        return R{err};
    }

    size_type words_out = std::min(static_cast<size_type>(tlv->length / sizeof(unsigned int)), buffer_words);
    memcpy(buffer, tlv->tlv, words_out * sizeof(unsigned int));
    free(raw);
    return R{0, words_out};
}

result mixer_ctl::set_tlv_raw(const unsigned int *buffer, size_type buffer_words) const noexcept {
    if (fd == invalid_fd()) return ENOENT;
    if (!has_tlv_write_) return ENOSYS;
    if (!buffer || buffer_words == 0) return EINVAL;

    size_type alloc_bytes = sizeof(snd_ctl_tlv) + buffer_words * sizeof(unsigned int);
    auto *raw = static_cast<unsigned char *>(malloc(alloc_bytes));
    if (!raw) return ENOMEM;

    auto *tlv = reinterpret_cast<snd_ctl_tlv *>(raw);
    tlv->numid = numid_;
    tlv->length = static_cast<unsigned int>(buffer_words * sizeof(unsigned int));
    memcpy(tlv->tlv, buffer, tlv->length);

    auto ioctl_err = ioctl(fd, SNDRV_CTL_IOCTL_TLV_WRITE, tlv);
    int err = errno;
    free(raw);
    if (ioctl_err < 0) return err;
    return result();
}

// ============================================================================
// mixer_impl
// ============================================================================

class mixer_impl {
    friend class mixer;
    int fd = invalid_fd();
    // We use raw storage + explicit construction so we never realloc a
    // non-trivially-copyable type (mixer_ctl declares a move constructor,
    // which makes realloc unsafe per [class.copy]).
    mixer_ctl *ctls = nullptr;
    size_type num_ctls = 0;
    size_type cap_ctls = 0;

    bool reserve(size_type n) noexcept {
        if (n <= cap_ctls) return true;
        auto *buf = static_cast<mixer_ctl *>(::operator new(n * sizeof(mixer_ctl), std::nothrow));
        if (!buf) return false;
        // Move-construct existing elements into new storage
        for (size_type i = 0; i < num_ctls; ++i) {
            new (buf + i) mixer_ctl(std::move(ctls[i]));
            ctls[i].~mixer_ctl();
        }
        ::operator delete(ctls);
        ctls = buf;
        cap_ctls = n;
        return true;
    }

    bool push_back(mixer_ctl &&ctl) noexcept {
        if (num_ctls == cap_ctls) {
            size_type new_cap = cap_ctls ? cap_ctls * 2 : 8;
            if (!reserve(new_cap)) return false;
        }
        new (ctls + num_ctls) mixer_ctl(std::move(ctl));
        ++num_ctls;
        return true;
    }
};

// ============================================================================
// mixer
// ============================================================================

mixer::mixer() noexcept
    : self(nullptr) {
}

mixer::mixer(mixer &&other) noexcept
    : self(other.self) {
    other.self = nullptr;
}

mixer &mixer::operator=(mixer &&other) noexcept {
    if (this != &other) {
        close();
        self = other.self;
        other.self = nullptr;
    }
    return *this;
}

mixer::~mixer() {
    close();
}

result mixer::open(size_type card) noexcept {
    close();
    self = new (std::nothrow) mixer_impl();
    if (!self) return {ENOMEM};

    char path[256];
    snprintf(path, sizeof(path), "/dev/snd/controlC%lu", (unsigned long)card);
    self->fd = ::open(path, O_RDWR | O_NONBLOCK);
    if (self->fd < 0) {
        delete self;
        self = nullptr;
        return {errno};
    }

    // Count controls
    snd_ctl_elem_list list{};
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        ::close(self->fd);
        delete self;
        self = nullptr;
        return {errno};
    }

    if (list.count == 0) return {0};

    // Pre-allocate storage to avoid repeated reallocations.
    if (!self->reserve(list.count)) {
        ::close(self->fd);
        delete self;
        self = nullptr;
        return {ENOMEM};
    }

    // Fetch all IDs
    auto *ids = static_cast<snd_ctl_elem_id *>(calloc(list.count, sizeof(snd_ctl_elem_id)));
    if (!ids) {
        ::close(self->fd);
        delete self;
        self = nullptr;
        return {ENOMEM};
    }

    list.offset = 0;
    list.space = list.count;
    list.pids = ids;
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        free(ids);
        ::close(self->fd);
        delete self;
        self = nullptr;
        return {errno};
    }

    // Fetch info for each control and build mixer_ctl objects
    for (unsigned int i = 0; i < list.used; ++i) {
        snd_ctl_elem_info info{};
        info.id = ids[i];
        if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) continue;

        mixer_ctl ctl;
        ctl.fd = self->fd;
        ctl.numid_ = info.id.numid;
        ctl.iface_ = info.id.iface;
        ctl.device_ = info.id.device;
        ctl.subdevice_ = info.id.subdevice;
        ctl.index_ = info.id.index;
        ctl.elem_type_ = info.type;
        ctl.count_ = info.count;
        ctl.has_tlv_ = (info.access & SNDRV_CTL_ELEM_ACCESS_TLV_READ) != 0;
        ctl.has_tlv_write_ = (info.access & SNDRV_CTL_ELEM_ACCESS_TLV_WRITE) != 0;
        strncpy(ctl.name_, reinterpret_cast<const char *>(info.id.name), sizeof(ctl.name_) - 1);
        ctl.name_[sizeof(ctl.name_) - 1] = '\0';

        if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            ctl.min_ = info.value.integer.min;
            ctl.max_ = info.value.integer.max;
        } else if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
            ctl.min64_ = info.value.integer64.min;
            ctl.max64_ = info.value.integer64.max;
        }

        // Load enum item names by querying each item index separately.
        if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED && info.value.enumerated.items > 0) {
            unsigned int n = info.value.enumerated.items;
            char *names = static_cast<char *>(calloc(n, kEnumNameLen));
            if (names) {
                unsigned int loaded = 0;
                for (unsigned int j = 0; j < n; ++j) {
                    snd_ctl_elem_info item_info{};
                    item_info.id = ids[i];
                    item_info.value.enumerated.item = j;
                    if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &item_info) == 0) {
                        strncpy(names + j * kEnumNameLen, item_info.value.enumerated.name, kEnumNameLen - 1);
                        names[j * kEnumNameLen + kEnumNameLen - 1] = '\0';
                        ++loaded;
                    }
                }
                if (loaded > 0) {
                    ctl.enum_names_ = names;
                    ctl.enum_items_count_ = n;
                } else {
                    free(names);
                }
            }
        }

        self->push_back(std::move(ctl));
    }

    free(ids);
    return {0};
}

void mixer::close() noexcept {
    if (!self) return;

    // Free enum name buffers and explicitly destruct each control before
    // releasing the raw storage (no implicit destructor calls with operator delete).
    for (size_type i = 0; i < self->num_ctls; ++i) {
        free(self->ctls[i].enum_names_);
        self->ctls[i].enum_names_ = nullptr;
        self->ctls[i].enum_items_count_ = 0;
        self->ctls[i].fd = invalid_fd();
        self->ctls[i].~mixer_ctl();
    }

    if (self->fd != invalid_fd()) ::close(self->fd);
    ::operator delete(self->ctls);
    self->ctls = nullptr;
    self->num_ctls = 0;
    self->cap_ctls = 0;
    delete self;
    self = nullptr;
}

bool mixer::is_open() const noexcept {
    return self && self->fd != invalid_fd();
}

const mixer_ctl *mixer::get_ctl_by_name(const char *name) const noexcept {
    if (!self) return nullptr;
    for (size_type i = 0; i < self->num_ctls; ++i) {
        if (strcmp(self->ctls[i].get_name(), name) == 0) return &self->ctls[i];
    }
    return nullptr;
}

size_type mixer::get_num_ctls() const noexcept {
    return self ? self->num_ctls : 0;
}

const mixer_ctl *mixer::get_ctl(size_type index) const noexcept {
    if (!self || index >= self->num_ctls) return nullptr;
    return &self->ctls[index];
}

int mixer::get_file_descriptor() const noexcept {
    return self ? self->fd : invalid_fd();
}

result mixer::subscribe_events(bool enable) noexcept {
    if (!self || self->fd == invalid_fd()) return {ENOENT};
    int flag = enable ? 1 : 0;
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &flag) < 0) return {errno};
    return {0};
}

generic_result<mixer_event> mixer::read_event() noexcept {
    using R = generic_result<mixer_event>;
    if (!self || self->fd == invalid_fd()) return R{ENOENT};

    snd_ctl_event ev{};
    ssize_t n = ::read(self->fd, &ev, sizeof(ev));
    if (n < 0) return R{errno};
    if (n == 0) return R{EAGAIN};

    mixer_event out;
    out.numid = ev.data.elem.id.numid;
    out.mask = ev.data.elem.mask;
    return R{0, out};
}

generic_result<protocol_version> mixer::get_protocol_version() const noexcept {
    using R = generic_result<protocol_version>;
    if (!self || self->fd == invalid_fd()) return R{ENOENT};
    int raw = 0;
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_PVERSION, &raw) < 0) return R{errno};

    protocol_version v;
    v.major = static_cast<unsigned int>(raw >> 16) & 0xffffu;
    v.minor = static_cast<unsigned int>(raw >> 8) & 0xffu;
    v.subminor = static_cast<unsigned int>(raw) & 0xffu;
    return R{0, v};
}

result mixer::add_integer_control(const char *name, long min, long max, long step, size_type count) noexcept {
    if (!self || self->fd == invalid_fd()) return {ENOENT};
    if (!name || count == 0 || count > 128) return {EINVAL};

    snd_ctl_elem_info info{};
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy(reinterpret_cast<char *>(info.id.name), name, sizeof(info.id.name) - 1);
    info.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    info.count = static_cast<unsigned int>(count);
    info.value.integer.min = min;
    info.value.integer.max = max;
    info.value.integer.step = step;
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_ADD, &info) < 0) return {errno};
    return {0};
}

result mixer::remove_control(const char *name) noexcept {
    if (!self || self->fd == invalid_fd()) return {ENOENT};
    const mixer_ctl *ctl = get_ctl_by_name(name);
    if (!ctl) return {ENOENT};

    snd_ctl_elem_id id{};
    id.numid = ctl->get_numid();
    if (ioctl(self->fd, SNDRV_CTL_IOCTL_ELEM_REMOVE, &id) < 0) return {errno};
    return {0};
}

} // namespace tinyalsa
