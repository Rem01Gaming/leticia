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
    u32_be
};

// ============================================================================
// sample_traits
// ============================================================================

namespace detail {

/**
 * @brief CRTP base providing compile-time traits for a sample format.
 *
 * @tparam IsSigned       Whether the format is signed.
 * @tparam BitDepth       Significant bits (e.g. 24 for S24_LE).
 * @tparam ContainerBytes Storage bytes per sample (e.g. 4 for S24_LE, 3 for S24_3LE).
 * @tparam LittleEndian   Byte order. For 8-bit formats @c true is returned by convention.
 */
template <bool IsSigned, int BitDepth, int ContainerBytes, bool LittleEndian>
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

/**
 * @brief Returns the number of bytes required to hold one audio frame.
 * @param fmt      The sample format.
 * @param channels Number of interleaved channels.
 * @return Bytes per frame, or 0 for an unrecognised format.
 */
size_type bytes_per_frame(sample_format fmt, size_type channels) noexcept;

/**
 * @brief Returns the ALSA string representation of a @ref sample_format value.
 * @return e.g. @c "S16_LE", @c "U24_3BE", or @c "UNKNOWN".
 */
const char *to_string(sample_format sf) noexcept;

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
    size_type buffer_frames_ = 0;
    size_type frame_bytes_ = 0;
    size_type appl_ptr_ = 0;
    size_type boundary_ = 0;

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
    size_type buffer_frames_ = 0;
    size_type frame_bytes_ = 0;
    size_type appl_ptr_ = 0;
    size_type boundary_ = 0;

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
    size_type count_ = 0;
    unsigned int elem_type_ = 0;
    unsigned int numid_ = 0;
    unsigned int iface_ = 0;
    unsigned int device_ = 0;
    unsigned int subdevice_ = 0;
    unsigned int index_ = 0;

    char *enum_names_ = nullptr;
    unsigned int enum_items_count_ = 0;

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
