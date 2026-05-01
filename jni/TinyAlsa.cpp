#include "TinyAlsa.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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
    return params;
}

size_type compute_boundary(size_type buffer_size) noexcept {
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

    const size_type buf = config.period_count * config.period_size;
    size_type boundary = buf;
    while (boundary * 2 <= static_cast<size_type>(LONG_MAX) - buf) boundary *= 2;
    params.boundary = boundary;

    params.xfer_align = config.period_size / 2;
    params.silence_size = 0;
    params.silence_threshold = config.silence_threshold;
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
    if (err < 0) return {errno, 0};
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
    if (err < 0) return {errno, 0};
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
    // POLLIN = 0x0001, POLLOUT = 0x0004
    return self->is_capture ? 0x0001 : 0x0004;
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

int mmap_setup_common(
    int fd,
    const pcm_config &config,
    sample_access access,
    bool is_capture,
    void **out_ptr,
    size_type *out_buffer_frames,
    size_type *out_frame_bytes,
    size_type *out_boundary
) noexcept {
    // Hardware parameters
    auto hw_params = to_alsa_hw_params(config, access);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw_params) < 0) return errno;

    // Software parameters
    auto sw_params = to_alsa_sw_params(config, is_capture);
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw_params) < 0) return errno;

    // Derive sizes from the requested (exact) config.
    // Because we set integer=1 with min==max, the negotiated values equal
    // what we requested, or HW_PARAMS would have failed.
    size_type buffer_frames = config.period_size * config.period_count;
    size_type frame_bytes = bytes_per_frame(config.format, config.channels);
    if (frame_bytes == 0) return EINVAL;

    size_type map_size = buffer_frames * frame_bytes;

    // Map the DMA data buffer (offset 0 = PCM data for modern ALSA drivers).
    void *ptr = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) return errno;

    *out_ptr = ptr;
    *out_buffer_frames = buffer_frames;
    *out_frame_bytes = frame_bytes;
    *out_boundary = compute_boundary(buffer_frames);
    return 0;
}

void mmap_teardown_common(void *ptr, size_type buffer_frames, size_type frame_bytes) noexcept {
    if (ptr && ptr != MAP_FAILED) ::munmap(ptr, buffer_frames * frame_bytes);
}

int mmap_begin_write(
    int fd,
    size_type appl_ptr,
    size_type buffer_frames,
    size_type boundary,
    size_type frame_bytes,
    void *mmap_data,
    mmap_region *out
) noexcept {
    snd_pcm_sync_ptr sptr{};
    sptr.flags = SNDRV_PCM_SYNC_PTR_HWSYNC;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0) return errno;

    snd_pcm_uframes_t hw_ptr = sptr.s.status.hw_ptr;

    // Frames used (written but not yet played)
    size_type used;
    if (appl_ptr >= (size_type)hw_ptr) {
        used = appl_ptr - (size_type)hw_ptr;
    } else {
        used = boundary - ((size_type)hw_ptr - appl_ptr);
    }

    if (used > buffer_frames) return EPIPE; // xrun

    size_type avail = buffer_frames - used;
    if (avail == 0) return EAGAIN;

    // Limit to contiguous region (don't cross ring-buffer end)
    size_type offset = appl_ptr % buffer_frames;
    size_type contig = buffer_frames - offset;
    if (avail > contig) avail = contig;

    out->data = static_cast<char *>(mmap_data) + offset * frame_bytes;
    out->offset = offset;
    out->avail = avail;
    return 0;
}

/**
 * Sync the hardware pointer and compute available frames for capture (read).
 * avail = hw_ptr - appl_ptr  [modulo boundary]
 */
int mmap_begin_read(
    int fd,
    size_type appl_ptr,
    size_type buffer_frames,
    size_type boundary,
    size_type frame_bytes,
    void *mmap_data,
    mmap_region *out
) noexcept {
    snd_pcm_sync_ptr sptr{};
    sptr.flags = SNDRV_PCM_SYNC_PTR_HWSYNC;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0) return errno;

    snd_pcm_uframes_t hw_ptr = sptr.s.status.hw_ptr;

    // Frames available to read (captured but not yet consumed)
    size_type avail;
    if ((size_type)hw_ptr >= appl_ptr) {
        avail = (size_type)hw_ptr - appl_ptr;
    } else {
        avail = boundary - (appl_ptr - (size_type)hw_ptr);
    }

    if (avail > buffer_frames) return EPIPE; // overrun
    if (avail == 0) return EAGAIN;

    size_type offset = appl_ptr % buffer_frames;
    size_type contig = buffer_frames - offset;
    if (avail > contig) avail = contig;

    out->data = static_cast<char *>(mmap_data) + offset * frame_bytes;
    out->offset = offset;
    out->avail = avail;
    return 0;
}

int mmap_commit_common(int fd, size_type *appl_ptr, size_type frames, size_type boundary) noexcept {
    *appl_ptr += frames;
    if (*appl_ptr >= boundary) *appl_ptr -= boundary;

    snd_pcm_sync_ptr sptr{};
    sptr.flags = SNDRV_PCM_SYNC_PTR_APPL;
    sptr.c.control.appl_ptr = *appl_ptr;
    sptr.c.control.avail_min = 1;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sptr) < 0) return errno;
    return 0;
}

} // namespace

// ============================================================================
// mmap_pcm_writer
// ============================================================================

mmap_pcm_writer::~mmap_pcm_writer() {
    mmap_teardown_common(mmap_data_, buffer_frames_, frame_bytes_);
}

result mmap_pcm_writer::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_playback_device(card, device, non_blocking);
}

result mmap_pcm_writer::setup(const pcm_config &config) noexcept {
    mmap_teardown_common(mmap_data_, buffer_frames_, frame_bytes_);
    mmap_data_ = nullptr;
    buffer_frames_ = 0;
    frame_bytes_ = 0;
    appl_ptr_ = 0;
    boundary_ = 0;

    int err = mmap_setup_common(
        get_file_descriptor(),
        config,
        sample_access::mmap_interleaved,
        false,
        &mmap_data_,
        &buffer_frames_,
        &frame_bytes_,
        &boundary_
    );
    if (err) return err;
    return 0;
}

generic_result<mmap_region> mmap_pcm_writer::begin() noexcept {
    using R = generic_result<mmap_region>;
    mmap_region rgn{};
    int err = mmap_begin_write(get_file_descriptor(), appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    if (err) return R{err};
    return R{0, rgn};
}

result mmap_pcm_writer::commit(size_type frames) noexcept {
    int err = mmap_commit_common(get_file_descriptor(), &appl_ptr_, frames, boundary_);
    if (err) return err;
    return 0;
}

// ============================================================================
// mmap_pcm_reader
// ============================================================================

mmap_pcm_reader::~mmap_pcm_reader() {
    mmap_teardown_common(mmap_data_, buffer_frames_, frame_bytes_);
}

result mmap_pcm_reader::open(size_type card, size_type device, bool non_blocking) noexcept {
    return pcm::open_capture_device(card, device, non_blocking);
}

result mmap_pcm_reader::setup(const pcm_config &config) noexcept {
    mmap_teardown_common(mmap_data_, buffer_frames_, frame_bytes_);
    mmap_data_ = nullptr;
    buffer_frames_ = 0;
    frame_bytes_ = 0;
    appl_ptr_ = 0;
    boundary_ = 0;

    int err = mmap_setup_common(
        get_file_descriptor(),
        config,
        sample_access::mmap_interleaved,
        true,
        &mmap_data_,
        &buffer_frames_,
        &frame_bytes_,
        &boundary_
    );
    if (err) return err;
    return 0;
}

generic_result<mmap_region> mmap_pcm_reader::begin() noexcept {
    using R = generic_result<mmap_region>;
    mmap_region rgn{};
    int err = mmap_begin_read(get_file_descriptor(), appl_ptr_, buffer_frames_, boundary_, frame_bytes_, mmap_data_, &rgn);
    if (err) return R{err};
    return R{0, rgn};
}

result mmap_pcm_reader::commit(size_type frames) noexcept {
    int err = mmap_commit_common(get_file_descriptor(), &appl_ptr_, frames, boundary_);
    if (err) return err;
    return 0;
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
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_rate(size_type rate) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_RATE>::set(test, rate);
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_channels(size_type ch) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_CHANNELS>::set(test, ch);
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_period_size(size_type ps) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_PERIOD_SIZE>::set(test, ps);
    return ioctl(self->fd, SNDRV_PCM_IOCTL_HW_REFINE, &test) >= 0;
}

bool pcm_params::test_period_count(size_type pc) const noexcept {
    if (!is_open()) return false;
    auto test = self->params;
    interval_ref<SNDRV_PCM_HW_PARAM_PERIODS>::set(test, pc);
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
        sample_format::u32_be,
    };

    for (auto fmt : all_formats) {
        // Use the already-refined params as a starting mask and pin only FORMAT.
        auto test = self->params;
        mask_ref<SNDRV_PCM_HW_PARAM_FORMAT>::set(test, to_alsa_format(fmt));
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
    if (err == EPIPE || err == ESTRPIPE || err == EAGAIN) return p.prepare();
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
    , count_(other.count_)
    , elem_type_(other.elem_type_)
    , numid_(other.numid_)
    , iface_(other.iface_)
    , device_(other.device_)
    , subdevice_(other.subdevice_)
    , index_(other.index_)
    , enum_names_(other.enum_names_)
    , enum_items_count_(other.enum_items_count_) {
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
        count_ = other.count_;
        elem_type_ = other.elem_type_;
        numid_ = other.numid_;
        iface_ = other.iface_;
        device_ = other.device_;
        subdevice_ = other.subdevice_;
        index_ = other.index_;
        enum_names_ = other.enum_names_;
        enum_items_count_ = other.enum_items_count_;
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
        strncpy(ctl.name_, reinterpret_cast<const char *>(info.id.name), sizeof(ctl.name_) - 1);
        ctl.name_[sizeof(ctl.name_) - 1] = '\0';

        if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            ctl.min_ = info.value.integer.min;
            ctl.max_ = info.value.integer.max;
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

} // namespace tinyalsa
