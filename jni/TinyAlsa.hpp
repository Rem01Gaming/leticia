#pragma once

#include <climits>
#include <cstddef>
#include <utility>

/**
 * @brief A lightweight C++ wrapper around the Linux ALSA (Advanced Linux Sound Architecture).
 */
namespace tinyalsa {

/**
 * @brief Unsigned size type used throughout the library (aliased from std::size_t).
 */
using size_type = std::size_t;

/**
 * @brief Converts an errno value to a human-readable string.
 */
const char *get_error_description(int error) noexcept;

// ============================================================================
// generic_result<T>
// ============================================================================

template <typename value_type>
struct generic_result final {
    int error = 0;
    value_type value = value_type();

    constexpr bool failed() const noexcept {
        return error != 0;
    }

    inline const char *error_description() const noexcept {
        return get_error_description(error);
    }

    value_type unwrap() noexcept {
        return std::move(value);
    }
};

template <>
struct generic_result<void> final {
    int error = 0;

    constexpr generic_result(int errno_copy = 0) noexcept
        : error(errno_copy) {
    }

    constexpr bool failed() const noexcept {
        return error != 0;
    }

    inline const char *error_description() const noexcept {
        return get_error_description(error);
    }
};

using result = generic_result<void>;

// ============================================================================
// Sentinel values
// ============================================================================

static constexpr int invalid_fd() noexcept {
    return -1;
}
static constexpr size_type invalid_card() noexcept {
    return 0xffff;
}
static constexpr size_type invalid_device() noexcept {
    return 0xffff;
}
static constexpr size_type invalid_subdevice() noexcept {
    return 0xffff;
}

// ============================================================================
// sample_format
// ============================================================================

enum class sample_format {
    s8,
    s16_le,
    s16_be,
    s18_3le,
    s18_3be,
    s20_3le,
    s20_3be,
    s24_3le,
    s24_3be,
    s24_le,
    s24_be,
    s32_le,
    s32_be,
    u8,
    u16_le,
    u16_be,
    u18_3le,
    u18_3be,
    u20_3le,
    u20_3be,
    u24_3le,
    u24_3be,
    u24_le,
    u24_be,
    u32_le,
    u32_be,
    dsd_u8,
    dsd_u16_le,
    dsd_u16_be,
    dsd_u32_le,
    dsd_u32_be
};

// ============================================================================
// sample_traits
// ============================================================================

namespace detail {

/**
 * @brief CRTP base providing compile-time traits for a sample format.
 *
 * @tparam IsSigned       Whether the format is signed.
 * @tparam BitDepth       Significant bits (e.g. 24 for S24_LE). For DSD formats this is the
 *                         number of packed 1-bit samples per container (= ContainerBytes * 8),
 *                         not a PCM signal resolution.
 * @tparam ContainerBytes Storage bytes per sample (e.g. 4 for S24_LE, 3 for S24_3LE).
 * @tparam LittleEndian   Byte order. For 8-bit formats @c true is returned by convention.
 * @tparam IsDsd          Whether the format is a DSD bitstream rather than linear PCM.
 */
template <bool IsSigned, int BitDepth, int ContainerBytes, bool LittleEndian, bool IsDsd = false>
struct sample_traits_base {
    static constexpr bool is_signed() noexcept {
        return IsSigned;
    }
    static constexpr int bit_depth() noexcept {
        return BitDepth;
    }
    static constexpr int container_bytes() noexcept {
        return ContainerBytes;
    }
    static constexpr bool is_little_endian() noexcept {
        return LittleEndian;
    }
    static constexpr bool is_dsd() noexcept {
        return IsDsd;
    }
};

} // namespace detail

/**
 * @brief Compile-time traits for a given @ref sample_format.
 */
template <sample_format sf>
struct sample_traits final {};

// 8-bit (endianness irrelevant, true by convention)
template <>
struct sample_traits<sample_format::s8> final : detail::sample_traits_base<true, 8, 1, true> {};
template <>
struct sample_traits<sample_format::u8> final : detail::sample_traits_base<false, 8, 1, true> {};

// 16-bit
template <>
struct sample_traits<sample_format::s16_le> final : detail::sample_traits_base<true, 16, 2, true> {};
template <>
struct sample_traits<sample_format::s16_be> final : detail::sample_traits_base<true, 16, 2, false> {};
template <>
struct sample_traits<sample_format::u16_le> final : detail::sample_traits_base<false, 16, 2, true> {};
template <>
struct sample_traits<sample_format::u16_be> final : detail::sample_traits_base<false, 16, 2, false> {};

// 18-bit packed in 3 bytes
template <>
struct sample_traits<sample_format::s18_3le> final : detail::sample_traits_base<true, 18, 3, true> {};
template <>
struct sample_traits<sample_format::s18_3be> final : detail::sample_traits_base<true, 18, 3, false> {};
template <>
struct sample_traits<sample_format::u18_3le> final : detail::sample_traits_base<false, 18, 3, true> {};
template <>
struct sample_traits<sample_format::u18_3be> final : detail::sample_traits_base<false, 18, 3, false> {};

// 20-bit packed in 3 bytes
template <>
struct sample_traits<sample_format::s20_3le> final : detail::sample_traits_base<true, 20, 3, true> {};
template <>
struct sample_traits<sample_format::s20_3be> final : detail::sample_traits_base<true, 20, 3, false> {};
template <>
struct sample_traits<sample_format::u20_3le> final : detail::sample_traits_base<false, 20, 3, true> {};
template <>
struct sample_traits<sample_format::u20_3be> final : detail::sample_traits_base<false, 20, 3, false> {};

// 24-bit packed in 3 bytes
template <>
struct sample_traits<sample_format::s24_3le> final : detail::sample_traits_base<true, 24, 3, true> {};
template <>
struct sample_traits<sample_format::s24_3be> final : detail::sample_traits_base<true, 24, 3, false> {};
template <>
struct sample_traits<sample_format::u24_3le> final : detail::sample_traits_base<false, 24, 3, true> {};
template <>
struct sample_traits<sample_format::u24_3be> final : detail::sample_traits_base<false, 24, 3, false> {};

// 24-bit in 4-byte container
template <>
struct sample_traits<sample_format::s24_le> final : detail::sample_traits_base<true, 24, 4, true> {};
template <>
struct sample_traits<sample_format::s24_be> final : detail::sample_traits_base<true, 24, 4, false> {};
template <>
struct sample_traits<sample_format::u24_le> final : detail::sample_traits_base<false, 24, 4, true> {};
template <>
struct sample_traits<sample_format::u24_be> final : detail::sample_traits_base<false, 24, 4, false> {};

// 32-bit
template <>
struct sample_traits<sample_format::s32_le> final : detail::sample_traits_base<true, 32, 4, true> {};
template <>
struct sample_traits<sample_format::s32_be> final : detail::sample_traits_base<true, 32, 4, false> {};
template <>
struct sample_traits<sample_format::u32_le> final : detail::sample_traits_base<false, 32, 4, true> {};
template <>
struct sample_traits<sample_format::u32_be> final : detail::sample_traits_base<false, 32, 4, false> {};

// DSD bitstream formats (1 bit per sample, packed into the container)
template <>
struct sample_traits<sample_format::dsd_u8> final : detail::sample_traits_base<false, 8, 1, true, true> {};
template <>
struct sample_traits<sample_format::dsd_u16_le> final : detail::sample_traits_base<false, 16, 2, true, true> {};
template <>
struct sample_traits<sample_format::dsd_u16_be> final : detail::sample_traits_base<false, 16, 2, false, true> {};
template <>
struct sample_traits<sample_format::dsd_u32_le> final : detail::sample_traits_base<false, 32, 4, true, true> {};
template <>
struct sample_traits<sample_format::dsd_u32_be> final : detail::sample_traits_base<false, 32, 4, false, true> {};

/**
 * @brief Returns the number of bytes required to hold one audio frame.
 * @param fmt      The sample format.
 * @param channels Number of interleaved channels.
 * @return Bytes per frame, or 0 for an unrecognised format.
 */
size_type bytes_per_frame(sample_format fmt, size_type channels) noexcept;

/**
 * @brief Returns the ALSA string representation of a @ref sample_format value.
 * @return e.g. @c "S16_LE", @c "U24_3BE", @c "DSD_U32_LE", or @c "UNKNOWN".
 */
const char *to_string(sample_format sf) noexcept;

/**
 * @brief Returns true if @p fmt is a DSD bitstream format rather than linear PCM.
 */
constexpr bool is_dsd(sample_format fmt) noexcept {
    switch (fmt) {
        case sample_format::dsd_u8:
        case sample_format::dsd_u16_le:
        case sample_format::dsd_u16_be:
        case sample_format::dsd_u32_le:
        case sample_format::dsd_u32_be: return true;
        default: return false;
    }
}

// ============================================================================
// sample_access
// ============================================================================

enum class sample_access {
    interleaved,
    non_interleaved,
    mmap_interleaved,
    mmap_non_interleaved
};

// ============================================================================
// pcm_class / pcm_subclass
// ============================================================================

enum class pcm_class {
    unknown,
    generic,
    multi_channel,
    modem,
    digitizer
};

inline constexpr const char *to_string(pcm_class c) noexcept;

enum class pcm_subclass {
    unknown,
    generic_mix,
    multi_channel_mix
};

inline constexpr const char *to_string(pcm_subclass subclass) noexcept;

// ============================================================================
// pcm_state
// ============================================================================

/**
 * @brief Runtime state of an ALSA PCM stream.
 */
enum class pcm_state {
    open,        ///< Device open, not yet configured
    setup,       ///< Hardware parameters applied, not yet prepared
    prepared,    ///< Ready to start
    running,     ///< Actively transferring data
    xrun,        ///< Buffer overrun (capture) or underrun (playback)
    draining,    ///< Draining pending playback frames before stop
    paused,      ///< Paused via pcm::pause()
    suspended,   ///< Suspended by power-management subsystem
    disconnected ///< Hardware has been disconnected
};

// ============================================================================
// pcm_config
// ============================================================================

struct pcm_config final {
    size_type channels = 2;
    size_type rate = 48000;
    size_type period_size = 1024;
    size_type period_count = 2;
    sample_format format = sample_format::s16_le;
    size_type start_threshold = 0;
    size_type stop_threshold = 0;
    size_type silence_threshold = 0;

    /**
     * @brief Requests SNDRV_PCM_HW_PARAMS_NORESAMPLE.
     * @note Fails setup() with EINVAL if the requested rate is not natively
     * supported and the driver would otherwise have resampled to reach it.
     */
    bool disable_resampling = false;

    /**
     * @brief Requests SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP.
     * @note Only takes effect if the device advertises SNDRV_PCM_INFO_NO_PERIOD_WAKEUP;
     * silently ignored otherwise. Combine with an external timer-driven poll loop,
     * since the driver will not wake the caller on period boundaries.
     */
    bool disable_period_wakeup = false;

    /**
     * @brief Requests SNDRV_PCM_TSTAMP_ENABLE so pcm::get_timestamp() reflects the
     * time of the most recent hardware pointer update rather than being left at zero.
     */
    bool enable_timestamps = false;
};

// ============================================================================
// pcm_info
// ============================================================================

struct pcm_info final {
    size_type card = invalid_card();
    size_type device = invalid_device();
    size_type subdevice = invalid_subdevice();

    pcm_class class_;
    pcm_subclass subclass;

    char id[64];
    char name[80];
    char subname[32];

    size_type subdevices_count = 0;
    size_type subdevices_available = 0;

    bool is_capture = false;
};

// ============================================================================
// protocol_version
// ============================================================================

/** @brief Decoded SNDRV_PROTOCOL_VERSION(major, minor, subminor) result from a PVERSION ioctl. */
struct protocol_version final {
    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned int subminor = 0;
};

// ============================================================================
// channel_position
// ============================================================================

/** @brief Spatial channel role, mirrors the kernel's SNDRV_CHMAP_* enum. */
enum class channel_position {
    unknown,
    na,
    mono,
    front_left,
    front_right,
    rear_left,
    rear_right,
    front_center,
    lfe,
    side_left,
    side_right,
    rear_center,
    front_left_center,
    front_right_center,
    rear_left_center,
    rear_right_center,
    front_left_wide,
    front_right_wide,
    front_left_high,
    front_center_high,
    front_right_high,
    top_center,
    top_front_left,
    top_front_right,
    top_front_center,
    top_rear_left,
    top_rear_right,
    top_rear_center,
    top_front_left_center,
    top_front_right_center,
    top_side_left,
    top_side_right,
    left_lfe,
    right_lfe,
    bottom_center,
    bottom_left_center,
    bottom_right_center
};

/** @brief Converts a raw SNDRV_CHMAP_* value (as read from a channel-map mixer control) to a @ref channel_position. */
channel_position to_channel_position(unsigned int raw_chmap_value) noexcept;

/** @brief Human-readable name for a @ref channel_position, e.g. "FL", "LFE". */
const char *to_string(channel_position pos) noexcept;

// ============================================================================
// pcm_channel_layout
// ============================================================================

/**
 * @brief Buffer layout of one channel within a non-interleaved DMA area, as
 * reported by SNDRV_PCM_IOCTL_CHANNEL_INFO.
 */
struct pcm_channel_layout final {
    long mmap_offset;   ///< Byte offset of the mmap region this channel lives in
    size_type first_bit; ///< Bit offset to the first sample
    size_type step_bits; ///< Bit distance between consecutive samples of this channel
};

// ============================================================================
// pcm
// ============================================================================

class pcm_impl;

/**
 * @brief Base class representing an open ALSA PCM device.
 */
class pcm {
    pcm_impl *self = nullptr;

public:
    pcm() noexcept;
    pcm(pcm &&other) noexcept;
    virtual ~pcm();

    int close() noexcept;
    int get_file_descriptor() const noexcept;
    bool is_open() const noexcept;

    generic_result<pcm_info> get_info() const noexcept;

    result prepare() noexcept;
    result start() noexcept;
    result drop() noexcept;
    result drain() noexcept;

    /**
     * @brief Pauses or resumes the stream without discarding buffered data.
     * @param enable @c true to pause, @c false to resume.
     * @note Not all drivers support pause, @c ENOSYS is returned if unsupported.
     * Check @c SND_PCM_INFO_PAUSE in the device info flags before relying on this.
     */
    result pause(bool enable) noexcept;

    /**
     * @brief Returns the current kernel state of the PCM stream.
     */
    generic_result<pcm_state> get_state() const noexcept;

    /**
     * @brief Returns available frames for I/O without blocking.
     */
    generic_result<size_type> get_avail() const noexcept;

    /**
     * @brief Returns the current stream latency in frames.
     */
    generic_result<long> get_delay() const noexcept;

    /**
     * @brief Returns the poll(2) event mask appropriate for this device.
     */
    int get_poll_events() const noexcept;

    /**
     * @brief Pushes the application pointer backward, replaying already-consumed frames.
     * @param frames Requested frame count to rewind.
     * @return Frames actually rewound (may be less than requested), or a negative errno.
     */
    generic_result<size_type> rewind(size_type frames) noexcept;

    /**
     * @brief Pushes the application pointer forward, skipping buffered frames.
     * @param frames Requested frame count to skip.
     * @return Frames actually skipped (may be less than requested), or a negative errno.
     */
    generic_result<size_type> forward(size_type frames) noexcept;

    /**
     * @brief Reports where one channel's samples live within the mmap'd DMA area.
     * @note Only meaningful once setup() has negotiated a non-interleaved access mode;
     * with interleaved access every channel reports the same interleaved layout.
     */
    generic_result<pcm_channel_layout> get_channel_info(size_type channel) const noexcept;

    /**
     * @brief Kernel PCM ioctl protocol version (SNDRV_PCM_IOCTL_PVERSION).
     */
    generic_result<protocol_version> get_protocol_version() const noexcept;

    /** @brief Reference timestamp of the most recent hardware pointer update. */
    struct timestamp final {
        long seconds = 0;
        long nanoseconds = 0;
    };

    /**
     * @brief Returns the timestamp attached to the last status refresh.
     * @note Only updated by the driver when pcm_config::enable_timestamps was set at setup() time.
     */
    generic_result<timestamp> get_timestamp() const noexcept;

    result open_capture_device(size_type card = 0, size_type device = 0, bool non_blocking = true) noexcept;
    result open_playback_device(size_type card = 0, size_type device = 0, bool non_blocking = true) noexcept;

protected:
    result setup(const pcm_config &config, sample_access access, bool is_capture) noexcept;
};

// ============================================================================
// interleaved_reader / interleaved_pcm_reader
// ============================================================================

class interleaved_reader {
public:
    virtual generic_result<size_type> read_unformatted(void *frames, size_type frame_count) noexcept = 0;
};

class interleaved_pcm_reader final : public pcm, public interleaved_reader {
public:
    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    inline result setup(const pcm_config &config = pcm_config()) noexcept {
        return pcm::setup(config, sample_access::interleaved, true);
    }

    generic_result<size_type> read_unformatted(void *frames, size_type frame_count) noexcept override;
};

// ============================================================================
// interleaved_writer / interleaved_pcm_writer
// ============================================================================

class interleaved_writer {
public:
    virtual generic_result<size_type> write_unformatted(const void *frames, size_type frame_count) noexcept = 0;
};

class interleaved_pcm_writer final : public pcm, public interleaved_writer {
public:
    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    inline result setup(const pcm_config &config = pcm_config()) noexcept {
        return pcm::setup(config, sample_access::interleaved, false);
    }

    generic_result<size_type> write_unformatted(const void *frames, size_type frame_count) noexcept override;
};

// ============================================================================
// noninterleaved_reader / noninterleaved_pcm_reader
// ============================================================================

class noninterleaved_reader {
public:
    /**
     * @param channel_buffers One destination pointer per channel, in channel order.
     * @param channel_count   Must equal the channel count negotiated at setup(); a
     * mismatch is rejected before the ioctl, since the kernel walks exactly
     * pcm_config::channels pointers and a short array causes an out-of-bounds read.
     */
    virtual generic_result<size_type> read_unformatted(void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept = 0;
};

class noninterleaved_pcm_reader final : public pcm, public noninterleaved_reader {
    size_type channels_ = 0;

public:
    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    inline result setup(const pcm_config &config = pcm_config()) noexcept {
        channels_ = config.channels;
        return pcm::setup(config, sample_access::non_interleaved, true);
    }

    generic_result<size_type> read_unformatted(void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept override;
};

// ============================================================================
// noninterleaved_writer / noninterleaved_pcm_writer
// ============================================================================

class noninterleaved_writer {
public:
    /**
     * @param channel_buffers One source pointer per channel, in channel order.
     * @param channel_count   Must equal the channel count negotiated at setup().
     */
    virtual generic_result<size_type> write_unformatted(const void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept = 0;
};

class noninterleaved_pcm_writer final : public pcm, public noninterleaved_writer {
    size_type channels_ = 0;

public:
    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    inline result setup(const pcm_config &config = pcm_config()) noexcept {
        channels_ = config.channels;
        return pcm::setup(config, sample_access::non_interleaved, false);
    }

    generic_result<size_type> write_unformatted(const void *const *channel_buffers, size_type channel_count, size_type frame_count) noexcept override;
};

// ============================================================================
// mmap_region
// ============================================================================

/**
 * @brief A contiguous slice of the hardware DMA ring buffer.
 */
struct mmap_region final {
    void *data;       ///< Pointer into the mapped DMA buffer
    size_type offset; ///< Frame offset within the ring buffer
    size_type avail;  ///< Maximum contiguous frames available in this slice
};

// ============================================================================
// mmap_pcm_writer
// ============================================================================

/**
 * @brief Zero-copy playback device using memory-mapped DMA access.
 */
class mmap_pcm_writer final : public pcm {
    void *mmap_data_ = nullptr;
    void *status_page_ = nullptr;
    void *control_page_ = nullptr;
    size_type buffer_frames_ = 0;
    size_type frame_bytes_ = 0;
    size_type appl_ptr_ = 0;
    size_type boundary_ = 0;
    size_type avail_min_ = 0;

    /** @brief True once status/control pages are mapped and SYNC_PTR ioctls can be skipped. */
    bool has_direct_pointers_ = false;

public:
    mmap_pcm_writer() noexcept = default;

    /** 
     * @brief Unmaps the DMA buffer and closes the device.
     */
    ~mmap_pcm_writer() override;

    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    /**
     * @brief Applies hardware parameters and maps the DMA buffer.
     * @note Must be called after open(). The DMA buffer covers config.period_size * config.period_count frames.
     */
    result setup(const pcm_config &config = pcm_config()) noexcept;

    /**
     * @brief Obtains a writable slice of the DMA ring buffer.
     *
     * @return mmap_region on success.
     *         EAGAIN if no space is available (non-blocking).
     *         EPIPE on underrun (call pcm_recover then retry).
     */
    generic_result<mmap_region> begin() noexcept;

    /**
     * @brief Advances the application pointer after writing @p frames frames.
     * @note frames must be <= the avail returned by the preceding begin().
     */
    result commit(size_type frames) noexcept;
};

// ============================================================================
// mmap_pcm_reader
// ============================================================================

/**
 * @brief Zero-copy capture device using memory-mapped DMA access.
 */
class mmap_pcm_reader final : public pcm {
    void *mmap_data_ = nullptr;
    void *status_page_ = nullptr;
    void *control_page_ = nullptr;
    size_type buffer_frames_ = 0;
    size_type frame_bytes_ = 0;
    size_type appl_ptr_ = 0;
    size_type boundary_ = 0;
    size_type avail_min_ = 0;

    /** @brief True once status/control pages are mapped and SYNC_PTR ioctls can be skipped. */
    bool has_direct_pointers_ = false;

public:
    mmap_pcm_reader() noexcept = default;

    /**
     * @brief Unmaps the DMA buffer and closes the device.
     */
    ~mmap_pcm_reader() override;

    result open(size_type card = 0, size_type device = 0, bool non_blocking = false) noexcept;

    /**
     * @brief Applies hardware parameters and maps the DMA buffer.
     */
    result setup(const pcm_config &config = pcm_config()) noexcept;

    /**
     * @brief Obtains a readable slice of the DMA ring buffer.
     * @return mmap_region on success.
     *         EAGAIN if no data is available yet (non-blocking).
     *         EPIPE on overrun (call pcm_recover then retry).
     */
    generic_result<mmap_region> begin() noexcept;

    /**
     * @brief Advances the application pointer after consuming @p frames frames.
     */
    result commit(size_type frames) noexcept;
};

// ============================================================================
// pcm_list
// ============================================================================

class pcm_list_impl;

class pcm_list final {
    pcm_list_impl *self = nullptr;

public:
    pcm_list() noexcept;
    pcm_list(pcm_list &&other) noexcept;
    ~pcm_list();

    size_type size() const noexcept;
    const pcm_info *data() const noexcept;

    inline const pcm_info &operator[](size_type index) const noexcept {
        return data()[index];
    }

    inline const pcm_info *begin() const noexcept {
        return data();
    }
    inline const pcm_info *end() const noexcept {
        return data() + size();
    }
};

// ============================================================================
// pcm_params
// ============================================================================

class pcm_params_impl;

/**
 * @brief Probes the hardware capabilities of a PCM device without configuring it.
 */
class pcm_params final {
    pcm_params_impl *self = nullptr;

public:
    pcm_params() noexcept;
    pcm_params(pcm_params &&other) noexcept;
    pcm_params &operator=(pcm_params &&other) noexcept;
    ~pcm_params();

    result open(size_type card, size_type device, bool is_capture) noexcept;
    void close() noexcept;
    bool is_open() const noexcept;

    bool test_format(sample_format fmt) const noexcept;
    bool test_rate(size_type rate) const noexcept;
    bool test_channels(size_type ch) const noexcept;
    bool test_period_size(size_type ps) const noexcept;
    bool test_period_count(size_type pc) const noexcept;

    /**
     * @brief Tests whether channels, rate, and format are all achievable together
     * in a single hw_params refine, rather than the three independently.
     * @note test_format()/test_rate()/test_channels() each refine only one axis
     * against the full device capability baseline, so a device that restricts a
     * format to certain rates (or vice versa) can pass each test individually
     * while the combination is not actually supported. This method is the
     * authoritative check for "would hw_params accept these three together" and
     * should be preferred whenever more than one axis is being pinned at once.
     */
    bool test_config(size_type channels, size_type rate, sample_format fmt) const noexcept;

    /**
     * @brief Iterates over every sample format the device actually supports.
     * @param callback  Called once per supported format. Must not be null.
     * @param user_data Forwarded unchanged to every callback invocation.
     */
    void for_each_supported_format(void (*callback)(sample_format, void *), void *user_data) const noexcept;

    /** @name Rate range */
    ///@{
    generic_result<size_type> get_min_rate() const noexcept;
    generic_result<size_type> get_max_rate() const noexcept;
    ///@}

    /** @name Channel count range */
    ///@{
    generic_result<size_type> get_min_channels() const noexcept;
    generic_result<size_type> get_max_channels() const noexcept;
    ///@}

    /** @name Period size range (frames) */
    ///@{
    generic_result<size_type> get_min_period_size() const noexcept;
    generic_result<size_type> get_max_period_size() const noexcept;
    ///@}

    /** @name Period count range */
    ///@{
    generic_result<size_type> get_min_period_count() const noexcept;
    generic_result<size_type> get_max_period_count() const noexcept;
    ///@}

    /**
     * @brief Returns the minimum total ring-buffer size in frames.
     */
    generic_result<size_type> get_min_buffer_size() const noexcept;

    /**
     * @brief Returns the maximum total ring-buffer size in frames.
     */
    generic_result<size_type> get_max_buffer_size() const noexcept;
};

// ============================================================================
// pcm_recover
// ============================================================================

result pcm_recover(pcm &p, int err, bool silent = false) noexcept;

// ============================================================================
// mixer_event
// ============================================================================

/**
 * @brief A change-notification from the ALSA control layer.
 */
struct mixer_event final {
    unsigned int numid; ///< Numeric ID of the changed control
    unsigned int mask;  ///< SNDRV_CTL_EVENT_MASK_VALUE, _INFO, _ADD, or _REMOVE
};

// ============================================================================
// mixer_ctl
// ============================================================================

/**
 * @brief A single ALSA mixer control element.
 */
class mixer_ctl {
    friend class mixer;

    int fd = invalid_fd();
    char name_[64] = {};
    long min_ = 0;
    long max_ = 0;
    long long min64_ = 0;
    long long max64_ = 0;
    size_type count_ = 0;
    unsigned int elem_type_ = 0;
    unsigned int numid_ = 0;
    unsigned int iface_ = 0;
    unsigned int device_ = 0;
    unsigned int subdevice_ = 0;
    unsigned int index_ = 0;

    char *enum_names_ = nullptr;
    unsigned int enum_items_count_ = 0;
    bool has_tlv_ = false;

    mixer_ctl() noexcept = default;

public:
    mixer_ctl(mixer_ctl &&other) noexcept;
    mixer_ctl &operator=(mixer_ctl &&other) noexcept;
    ~mixer_ctl() = default;

    // Identity

    const char *get_name() const noexcept {
        return name_;
    }
    unsigned int get_numid() const noexcept {
        return numid_;
    }
    size_type get_num_values() const noexcept {
        return count_;
    }

    // Type predicates

    /** @brief True if this is a volume-style integer control (min != max). */
    bool is_volume() const noexcept;
    /** @brief True if element type is SNDRV_CTL_ELEM_TYPE_INTEGER. */
    bool is_integer() const noexcept;
    /** @brief True if element type is SNDRV_CTL_ELEM_TYPE_BOOLEAN. */
    bool is_boolean() const noexcept;
    /** @brief True if element type is SNDRV_CTL_ELEM_TYPE_ENUMERATED. */
    bool is_enum() const noexcept;
    /** @brief True if element type is SNDRV_CTL_ELEM_TYPE_BYTES. */
    bool is_bytes() const noexcept;
    /** @brief True if element type is SNDRV_CTL_ELEM_TYPE_INTEGER64. */
    bool is_integer64() const noexcept;

    // Integer accessors

    generic_result<long> get_min() const noexcept {
        return {0, min_};
    }
    generic_result<long> get_max() const noexcept {
        return {0, max_};
    }

    /**
     * @brief Reads one integer element.
     * @param index Element index (0 = left for stereo volumes).
     * @return Value, or EINVAL if type is not integer or index is out of range.
     */
    generic_result<long> get_value(size_type index = 0) const noexcept;

    /**
     * @brief Writes one integer element.
     * @param value Should be within [get_min(), get_max()].
     */
    result set_value(long value, size_type index = 0) const noexcept;

    /** @brief Writes @p value to every integer element. */
    result set_all_values(long value) const noexcept;

    // 64-bit integer accessors

    generic_result<long long> get_min64() const noexcept {
        return {0, min64_};
    }
    generic_result<long long> get_max64() const noexcept {
        return {0, max64_};
    }

    /**
     * @brief Reads one 64-bit integer element.
     * @param index Element index.
     * @return Value, or EINVAL if type is not SNDRV_CTL_ELEM_TYPE_INTEGER64 or index is out of range.
     */
    generic_result<long long> get_int64(size_type index = 0) const noexcept;

    /**
     * @brief Writes one 64-bit integer element.
     * @param value Should be within [get_min64(), get_max64()].
     */
    result set_int64(long long value, size_type index = 0) const noexcept;

    /** @brief Writes @p value to every 64-bit integer element. */
    result set_all_int64(long long value) const noexcept;

    // Boolean accessors

    /**
     * @brief Reads one boolean element (on/off switch).
     * @return true/false, or EINVAL if type is not boolean.
     */
    generic_result<bool> get_bool(size_type index = 0) const noexcept;

    /**
     * @brief Writes one boolean element.
     */
    result set_bool(bool value, size_type index = 0) const noexcept;

    /** @brief Writes @p value to every boolean element. */
    result set_all_bools(bool value) const noexcept;

    // Enumerated accessors

    /** @brief Number of items in the enumeration (0 if not an enum control). */
    unsigned int get_num_enum_items() const noexcept {
        return enum_items_count_;
    }

    /**
     * @brief Name string for enumeration item @p item.
     * @return Null-terminated string, or nullptr if item is out of range.
     */
    const char *get_enum_item_name(unsigned int item) const noexcept;

    /**
     * @brief Reads the currently selected item index for one element.
     * @return Item index, or EINVAL if type is not enumerated.
     */
    generic_result<size_type> get_enum_index(size_type index = 0) const noexcept;

    /**
     * @brief Selects an item by index for one element.
     * @param item Must be < get_num_enum_items().
     */
    result set_enum_index(unsigned int item, size_type index = 0) const noexcept;

    /**
     * @brief Selects an item by exact name (case-sensitive) for one element.
     * @return EINVAL if the name is not found.
     */
    result set_enum_by_name(const char *name, size_type index = 0) const noexcept;

    /** @brief Sets every element to item index @p item. */
    result set_all_enum_indices(unsigned int item) const noexcept;

    // Bytes accessors

    /**
     * @brief Reads one byte from a bytes-type control.
     * @return The byte value, or EINVAL if type is not bytes or index out of range.
     */
    generic_result<unsigned char> get_byte(size_type index = 0) const noexcept;

    /**
     * @brief Writes one byte to a bytes-type control.
     */
    result set_byte(unsigned char value, size_type index = 0) const noexcept;

    // TLV / dB range accessors

    /** @brief True if the driver publishes TLV metadata for this control (SNDRV_CTL_ELEM_ACCESS_TLV_READ). */
    bool has_tlv() const noexcept {
        return has_tlv_;
    }

    /** @brief Decoded dB range for a volume control, from its SNDRV_CTL_TLVT_DB_SCALE/DB_MINMAX(_MUTE) TLV. */
    struct dB_range final {
        long min_millibel = 0;  ///< Volume at get_min(), in hundredths of a dB
        long max_millibel = 0;  ///< Volume at get_max(), in hundredths of a dB
        long step_millibel = 0; ///< dB step per integer step (0 for the MINMAX shapes, which are non-linear)
        bool has_mute = false;  ///< True if get_min() also mutes the control
    };

    /**
     * @brief Reads and decodes this control's dB-scale TLV, if it has one.
     * @return dB_range on success.
     *         ENOSYS if has_tlv() is false.
     *         ENOTSUP if the TLV is a shape this wrapper doesn't decode
     *         (e.g. SNDRV_CTL_TLVT_DB_RANGE or SNDRV_CTL_TLVT_DB_LINEAR); use
     *         get_tlv_raw() to inspect those directly.
     */
    generic_result<dB_range> get_dB_range() const noexcept;

    /**
     * @brief Reads the raw TLV blob (SNDRV_CTL_IOCTL_TLV_READ) for control types this
     * wrapper doesn't interpret itself, such as channel-map descriptors
     * (SNDRV_CTL_TLVT_CHMAP_*).
     * @param buffer       Destination for the raw 32-bit TLV words (type, length, payload...).
     * @param buffer_words Capacity of @p buffer in 32-bit words.
     * @return Number of words actually written into @p buffer.
     */
    generic_result<size_type> get_tlv_raw(unsigned int *buffer, size_type buffer_words) const noexcept;
};

// ============================================================================
// mixer
// ============================================================================

class mixer_impl;

/**
 * @brief Manages access to all ALSA mixer controls on a sound card.
 */
class mixer final {
    mixer_impl *self = nullptr;

public:
    mixer() noexcept;
    mixer(mixer &&other) noexcept;
    mixer &operator=(mixer &&other) noexcept;
    ~mixer();

    result open(size_type card) noexcept;
    void close() noexcept;
    bool is_open() const noexcept;

    const mixer_ctl *get_ctl_by_name(const char *name) const noexcept;
    size_type get_num_ctls() const noexcept;
    const mixer_ctl *get_ctl(size_type index) const noexcept;

    /**
     * @brief Returns the raw file descriptor for the control device.
     */
    int get_file_descriptor() const noexcept;

    /**
     * @brief Enables or disables change-notification events.
     */
    result subscribe_events(bool enable) noexcept;

    /**
     * @brief Reads one pending change-notification event.
     * @return mixer_event on success; EAGAIN when no more events are pending;
     *         ENOENT if the mixer is not open.
     */
    generic_result<mixer_event> read_event() noexcept;

    /** @brief Kernel control ioctl protocol version (SNDRV_CTL_IOCTL_PVERSION). */
    generic_result<protocol_version> get_protocol_version() const noexcept;

    /**
     * @brief Creates a user-defined integer control (SNDRV_CTL_IOCTL_ELEM_ADD).
     * @note Takes effect on the card immediately, but this mixer's own cached control
     * list is not refreshed; call open() again to see the new control through
     * get_ctl_by_name()/get_ctl().
     */
    result add_integer_control(const char *name, long min, long max, long step = 1, size_type count = 1) noexcept;

    /**
     * @brief Removes a control previously created with add_integer_control().
     * @note Only user-defined controls (SNDRV_CTL_ELEM_ACCESS_USER) can be removed;
     * driver-owned controls return EINVAL, matching the kernel's own restriction.
     */
    result remove_control(const char *name) noexcept;
};

// ============================================================================
// Stream operators
// ============================================================================

template <typename stream_type, typename value_type>
stream_type &operator<<(stream_type &output, const generic_result<value_type> &res) {
    if (res.failed()) return output << get_error_description(res.error);
    else
        return output << res.value;
}

template <typename stream_type>
stream_type &operator<<(stream_type &output, const result &res) {
    return output << get_error_description(res.error);
}

template <typename stream_type>
stream_type &operator<<(stream_type &output, pcm_class class_) {
    return output << to_string(class_);
}

template <typename stream_type>
stream_type &operator<<(stream_type &output, pcm_subclass subclass) {
    return output << to_string(subclass);
}

template <typename stream_type>
stream_type &operator<<(stream_type &output, const pcm_info &info) {
    output << "card      : " << info.card << '\n';
    output << "device    : " << info.device << '\n';
    output << "subdevice : " << info.subdevice << '\n';
    output << "class     : " << info.class_ << '\n';
    output << "subclass  : " << info.subclass << '\n';
    output << "id        : " << info.id << '\n';
    output << "name:     : " << info.name << '\n';
    output << "subname   : " << info.subname << '\n';
    output << "subdevices count     : " << info.subdevices_count << '\n';
    output << "subdevices available : " << info.subdevices_available << '\n';
    return output;
}

inline constexpr const char *to_string(pcm_class c) noexcept {
    switch (c) {
        case pcm_class::generic: return "Generic";
        case pcm_class::multi_channel: return "Multi-channel";
        case pcm_class::modem: return "Modem";
        case pcm_class::digitizer: return "Digitizer";
        default: break;
    }
    return "Unknown";
}

inline constexpr const char *to_string(pcm_subclass subclass) noexcept {
    switch (subclass) {
        case pcm_subclass::generic_mix: return "Generic Mix";
        case pcm_subclass::multi_channel_mix: return "Multi-channel Mix";
        default: break;
    }
    return "Unknown";
}

} // namespace tinyalsa
