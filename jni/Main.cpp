#include "Alsa.hpp"
#include "AnsiColors.hpp"
#include "DsdPacker.hpp"
#include "Replaygain.hpp"
#include "Resampler.hpp"
#include "SoftwareMixer.hpp"
#include "TinyAlsa.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

// ─── Global state ──────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_device_disconnected{false};
static std::atomic<float> g_volume{0.8f};      // Default 80%
static std::atomic<int64_t> g_seek_target{-1}; // -1 = no seek, -4 = toggle pause
static std::mutex g_print_mutex;

// TinyALSA Mixer Globals
static std::mutex g_mixer_mutex;
static tinyalsa::mixer g_mixer;
static const tinyalsa::mixer_ctl *g_hw_ctl = nullptr;
static bool g_hw_mixer_active = false;

// ─── Terminal raw-mode RAII ───────────────────────────────────────────────────
struct RawTerminal {
    termios saved{};
    bool active = false;

    void enable() {
        if (tcgetattr(STDIN_FILENO, &saved) < 0) return;
        termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) >= 0) {
            active = true;
        }
    }

    ~RawTerminal() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

// ─── Signal handler ───────────────────────────────────────────────────────────
static void on_signal(int) {
    g_running = false;
}

// ─── Volume persistence ──────────────────────────────────────────────────────
static std::string get_volume_file() {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }

    if (!home) return "/tmp/.leticia_volume";
    return std::string(home) + "/.leticia_volume";
}

static float load_volume() {
    std::string path = get_volume_file();
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return 0.8f;
    float vol = 0.8f;
    std::fscanf(f, "%f", &vol);
    std::fclose(f);
    return std::clamp(vol, 0.0f, 1.0f);
}

static void save_volume(float vol) {
    vol = std::clamp(vol, 0.0f, 1.0f);
    std::string path = get_volume_file();
    std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "w");
    if (f) {
        std::fprintf(f, "%.3f\n", vol);
        std::fclose(f);
        std::rename(tmp.c_str(), path.c_str());
    }
}

// ─── Utility: progress bar ────────────────────────────────────────────────────
static void print_status(double pos_sec, double dur_sec, float vol) {
    std::lock_guard<std::mutex> lk(g_print_mutex);

    int pos_m = (int)pos_sec / 60, pos_s = (int)pos_sec % 60;
    int dur_m = (int)dur_sec / 60, dur_s = (int)dur_sec % 60;

    const int bar_w = 30;
    double ratio = (dur_sec > 0) ? pos_sec / dur_sec : 0.0;
    int filled = (int)(ratio * bar_w);

    std::string bar = std::string(filled, '=') + (filled < bar_w ? ">" : "") + std::string(std::max(0, bar_w - filled - 1), ' ');

    int vol_pct = (int)(vol * 100.0f + 0.5f);

    auto fmt2 = [](int v) -> std::string {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", v);
        return buf;
    };

    std::cout << "\r" << CYAN << "[" << bar << "] " << WHITE << fmt2(pos_m) << ":" << fmt2(pos_s) << "/" << fmt2(dur_m) << ":"
              << fmt2(dur_s) << "  " << YELLOW << "VOL:" << vol_pct << "%  " << RESET << std::flush;
}

// ─── Hardware Mixer Setup & Control ───────────────────────────────────────────
static bool init_hw_mixer(tinyalsa::size_type card) {
    std::lock_guard<std::mutex> lock(g_mixer_mutex);
    if (g_mixer.open(card).failed()) return false;

    const char *targets[] = {
        "PCM Playback Volume", "Master Playback Volume", "Speaker Playback Volume", "PCM", "Master", "Speaker"
    };

    for (const char *name : targets) {
        auto ctl = g_mixer.get_ctl_by_name(name);
        if (ctl && ctl->is_volume()) {
            g_hw_ctl = ctl;
            g_hw_mixer_active = true;
            return true;
        }
    }
    return false;
}

static void cleanup_hw_mixer() {
    std::lock_guard<std::mutex> lock(g_mixer_mutex);
    g_hw_ctl = nullptr;
    g_hw_mixer_active = false;
    g_mixer.close();
}

static void set_volume(float new_vol) {
    new_vol = std::clamp(new_vol, 0.0f, 1.0f);
    g_volume.store(new_vol);
    save_volume(new_vol);

    std::lock_guard<std::mutex> lock(g_mixer_mutex);
    if (g_hw_mixer_active && g_hw_ctl) {
        auto min_r = g_hw_ctl->get_min();
        auto max_r = g_hw_ctl->get_max();
        if (!min_r.failed() && !max_r.failed()) {
            long hw_min = min_r.unwrap();
            long hw_max = max_r.unwrap();
            long hw_val = hw_min + static_cast<long>(new_vol * (hw_max - hw_min));
            g_hw_ctl->set_all_values(hw_val);
        }
    }
}

// ─── DSD codec detection ──────────────────────────────────────────────────────
static bool is_dsd_codec(enum AVCodecID codec_id) {
    return (
        codec_id == AV_CODEC_ID_DSD_LSBF || codec_id == AV_CODEC_ID_DSD_MSBF || codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR ||
        codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR
    );
}

// DSF (*_PLANAR) stores fixed-size per-channel blocks; DSDIFF (non-planar) is byte-interleaved.
// This is only meaningful for raw DoP / native DSD playback, which never goes through avcodec's
// dsd_lsbf/dsd_msbf decoder and must reconstruct the layout itself from the raw demuxed bytes.
static DsdSourceLayout make_dsd_layout(enum AVCodecID codec_id, int channels) {
    DsdSourceLayout layout;
    layout.channels = channels > 0 ? channels : 2;
    layout.msb_first = (codec_id == AV_CODEC_ID_DSD_MSBF || codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR);
    layout.planar = (codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR || codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR);
    layout.block_size = 4096; // Sony DSF spec default; ffmpeg's dsf demuxer packages one full block group per packet.
    return layout;
}

// ─── Keyboard thread ──────────────────────────────────────────────────────────
static void keyboard_thread() {
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeval tv{0, 200000};
        int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        char c = 0;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;

        if (c == 'q' || c == 'Q' || c == 3) {
            g_running = false;
            break;
        }

        if (c == ' ') {
            g_seek_target = -4; // Toggle pause
        }

        if (c == '\033') {
            char seq[2] = {};
            fd_set f2;
            FD_ZERO(&f2);
            FD_SET(STDIN_FILENO, &f2);
            timeval t2{0, 50000};

            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0) {
                ssize_t _r = read(STDIN_FILENO, &seq[0], 1);
                (void)_r;
            }

            FD_ZERO(&f2);
            FD_SET(STDIN_FILENO, &f2);
            t2 = {0, 50000};
            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0) {
                ssize_t _r = read(STDIN_FILENO, &seq[1], 1);
                (void)_r;
            }

            if (seq[0] == '[') {
                if (seq[1] == 'C') {
                    g_seek_target = -2;
                } else if (seq[1] == 'D') {
                    g_seek_target = -3;
                } else if (seq[1] == 'A') {
                    float v = g_volume.load();
                    set_volume(v + 0.02f);
                } else if (seq[1] == 'B') {
                    float v = g_volume.load();
                    set_volume(v - 0.02f);
                }
            }
        }
    }
}

// ─── Watcher thread using Mixer FD polling ────────────────────────────────────
static void watch_mixer_thread() {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_mixer_mutex);
        if (!g_mixer.is_open()) return;
        g_mixer.subscribe_events(true);
        fd = g_mixer.get_file_descriptor();
    }

    if (fd < 0) return;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (g_running) {
        int ret = poll(&pfd, 1, 500);
        if (ret > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                g_device_disconnected = true;
                g_running = false;
                break;
            }
            if (pfd.revents & POLLIN) {
                std::lock_guard<std::mutex> lock(g_mixer_mutex);
                while (!g_mixer.read_event().failed()) {} // Drain events
            }
        } else if (ret < 0 && errno == ENODEV) {
            g_device_disconnected = true;
            g_running = false;
            break;
        }
    }
}

// ─── Raw DSD playback ────────────
static int play_dsd_raw(const std::string &file_path, const AlsaDevice &dev, DsdOutputMode mode) {
    tinyalsa::size_type card = dev.card;
    tinyalsa::size_type device = dev.device;
    std::string display_name = dev.name + " [" + dev.hw_id + "]";

    bool hw_mixer_available = init_hw_mixer(card);
    set_volume(g_volume.load());

    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, file_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << RED << "ERROR: Cannot open file: " << file_path << RESET << "\n";
        return 1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << RED << "ERROR: Cannot find stream info.\n" << RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = (int)i;
            break;
        }
    }
    if (audio_idx < 0) {
        std::cerr << RED << "ERROR: No audio stream found.\n" << RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream *stream = fmt_ctx->streams[audio_idx];
    AVCodecParameters *codecpar = stream->codecpar;
    double duration_sec = (stream->duration != AV_NOPTS_VALUE) ? stream->duration * av_q2d(stream->time_base) :
                                                                 (double)fmt_ctx->duration / AV_TIME_BASE;

    int channels = codecpar->ch_layout.nb_channels > 0 ? codecpar->ch_layout.nb_channels : 2;
    tinyalsa::size_type dsd_rate = static_cast<tinyalsa::size_type>(codecpar->sample_rate);
    DsdSourceLayout layout = make_dsd_layout(codecpar->codec_id, channels);

    tinyalsa::pcm_params params;
    if (params.open(card, device, false).failed()) {
        std::cerr << RED << "ERROR: Cannot open PCM params for hw:" << card << "," << device << RESET << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    tinyalsa::sample_format out_fmt = tinyalsa::sample_format::s24_le;
    tinyalsa::sample_format native_fmt = tinyalsa::sample_format::dsd_u32_be;
    bool widen_dop_to_32bit = false;

    if (mode == DsdOutputMode::Native) {
        DsdOutputMode confirmed = choose_dsd_output_mode(params, dsd_rate, /*allowNative=*/true, /*allowDop=*/false, &native_fmt);
        if (confirmed != DsdOutputMode::Native) {
            std::cerr << RED << "ERROR: Device does not support the negotiated DSD configuration.\n" << RESET;
            params.close();
            avformat_close_input(&fmt_ctx);
            return 1;
        }
        out_fmt = native_fmt;
    } else {
        // dsd_rate is FFmpeg's dsf/dsdiff sample_rate: an already-halved byte rate
        // (bit rate / 8), so DoP's 2-bytes-per-word packing gives dop_rate = dsd_rate / 2.
        // See the matching note in DsdPacker.cpp::choose_dsd_output_mode.
        tinyalsa::size_type dop_rate = dsd_rate / 2;
        if (params.test_config((tinyalsa::size_type)channels, dop_rate, tinyalsa::sample_format::s24_le)) {
            out_fmt = tinyalsa::sample_format::s24_le;
        } else if (params.test_config((tinyalsa::size_type)channels, dop_rate, tinyalsa::sample_format::s32_le)) {
            out_fmt = tinyalsa::sample_format::s32_le;
            widen_dop_to_32bit = true;
        } else {
            std::cerr << RED << "ERROR: Device does not support the negotiated DSD configuration.\n" << RESET;
            params.close();
            avformat_close_input(&fmt_ctx);
            return 1;
        }
    }

    tinyalsa::size_type out_rate = dsd_pcm_rate(mode, dsd_rate, native_fmt);
    if (!params.test_config((tinyalsa::size_type)channels, out_rate, out_fmt)) {
        std::cerr << RED << "ERROR: Device does not support the negotiated DSD configuration.\n" << RESET;
        params.close();
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    tinyalsa::size_type period_size = (out_rate >= 176400) ? 4096 : 1024;
    auto min_period = params.get_min_period_size();
    auto max_period = params.get_max_period_size();
    tinyalsa::size_type period_lo = 32, period_hi = 8192;
    if (!min_period.failed() && !max_period.failed()) {
        period_lo = min_period.unwrap();
        period_hi = max_period.unwrap();
        period_size = std::clamp(period_size, period_lo, period_hi);

        // Round down to a power of two within range; most drivers prefer this,
        // and it matches the negotiation used by the regular PCM playback path.
        tinyalsa::size_type p = 1;
        while (p * 2 <= period_size) p *= 2;
        if (p >= period_lo) period_size = p;
    }
    params.close();

    tinyalsa::interleaved_pcm_writer writer;
    if (writer.open(card, device, false).failed()) {
        std::cerr << RED << "ERROR: Cannot open PCM device hw:" << card << "," << device << RESET << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    tinyalsa::pcm_config config;
    config.channels = (tinyalsa::size_type)channels;
    config.rate = out_rate;
    config.format = out_fmt;

    static constexpr std::array<tinyalsa::size_type, 4> kPeriodCountCandidates{8, 4, 3, 2};

    bool configured = false;
    int last_errno = 0;
    tinyalsa::size_type last_size = 0;
    tinyalsa::size_type last_count = 0;
    bool last_failed_at_prepare = false;
    for (tinyalsa::size_type count : kPeriodCountCandidates) {
        config.period_count = count;
        for (tinyalsa::size_type candidate = period_size; candidate >= 32; candidate /= 2) {
            if (candidate < period_lo || candidate > period_hi) continue;
            config.period_size = candidate;
            last_size = candidate;
            last_count = count;
            auto setup_result = writer.setup(config);
            if (setup_result.failed()) {
                last_errno = setup_result.error;
                last_failed_at_prepare = false;
                continue;
            }
            auto prepare_result = writer.prepare();
            if (prepare_result.failed()) {
                last_errno = prepare_result.error;
                last_failed_at_prepare = true;
                continue;
            }
            configured = true;
            break;
        }
        if (configured) break;
    }

    if (!configured) {
        std::cerr << RED << "ERROR: PCM configuration failed"
                  << " (channels=" << channels << " rate=" << out_rate
                  << " format=" << tinyalsa::to_string(out_fmt)
                  << ", last tried period_size=" << last_size << " period_count=" << last_count
                  << ", failed at " << (last_failed_at_prepare ? "prepare" : "hw_params")
                  << ", errno=" << last_errno << " [" << tinyalsa::get_error_description(last_errno) << "])\n" << RESET;
        writer.close();
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        // clang-format off
        std::cout << "\n"
                  << BOLD << CYAN
                  << "╔═════════════════════════════════════════════════════╗\n"
                  << "║                       Leticia                       ║\n"
                  << "╚═════════════════════════════════════════════════════╝\n"
                  << RESET << GREEN << "  File   : " << WHITE << file_path << "\n"
                  << RESET << GREEN << "  Device : " << WHITE << display_name << "\n"
                  << RESET << GREEN << "  Mixer  : " << WHITE << (hw_mixer_available ? "Hardware" : "Unavailable (raw DSD passthrough)") << "\n"
                  << RESET << GREEN << "  Rate   : " << WHITE << out_rate << " Hz  " << channels << " ch  "
                  << tinyalsa::to_string(out_fmt) << "\n"
                  << RESET << GREEN << "  Flags  : " << WHITE << "BitPerfect " << (mode == DsdOutputMode::Native ? "NativeDSD" : "DoP") << "\n\n"
                  << RESET << YELLOW << "  ← / → : Seek   ↑ / ↓ : Volume   SPACE : Pause   q : Quit\n"
                  << RESET << "\n";
        // clang-format on
    }

    std::thread watcher(watch_mixer_thread);
    std::thread kb(keyboard_thread);
    watcher.detach();

    DopPacker dop_packer(layout, widen_dop_to_32bit);
    NativeDsdPacker native_packer(layout, native_fmt);
    const size_t out_frame_bytes = tinyalsa::bytes_per_frame(out_fmt, (tinyalsa::size_type)channels);

    AVPacket *pkt = av_packet_alloc();
    std::vector<uint8_t> out_buf;
    double current_pos_sec = 0.0;
    bool is_paused = false;

    constexpr auto kSeekDebounce = std::chrono::milliseconds(250);
    constexpr double kSeekStepSec = 5.0;
    bool seek_pending = false;
    double seek_preview_sec = 0.0;
    std::chrono::steady_clock::time_point last_seek_key_time{};
    bool device_disconnected_local = false;

    // Pre-roll discard threshold: after a seek, packets with pts below this
    // value are discarded to allow the demuxer to stabilize and avoid sending
    // partial/misaligned DSD data that causes glitches.
    int64_t skip_until_pts = AV_NOPTS_VALUE;

    // Track if we're in the immediate post-seek period where we need to flush
    // the ALSA hardware buffer to prevent stale/corrupt audio from playing.
    bool post_seek_flush_pending = false;

    while (g_running) {
        int64_t seek_req = g_seek_target.exchange(-1);
        if (seek_req == -4) {
            is_paused = !is_paused;
            writer.pause(is_paused);
        } else if (seek_req == -2 || seek_req == -3) {
            double offset = (seek_req == -2) ? kSeekStepSec : -kSeekStepSec;
            double base = seek_pending ? seek_preview_sec : current_pos_sec;
            seek_preview_sec = std::max(0.0, base + offset);
            seek_pending = true;
            last_seek_key_time = std::chrono::steady_clock::now();
        }

        if (seek_pending && (std::chrono::steady_clock::now() - last_seek_key_time) >= kSeekDebounce) {
            double target_sec = seek_preview_sec;
            int64_t target_pts = static_cast<int64_t>(target_sec * AV_TIME_BASE);
            if (avformat_seek_file(fmt_ctx, -1, INT64_MIN, target_pts, INT64_MAX, 0) >= 0) {
                writer.prepare();
                // Reset DoP marker state to prevent glitch on resume after seek
                if (mode != DsdOutputMode::Native) {
                    dop_packer.reset();
                }
                current_pos_sec = target_sec;

                /*
                 * Convert the seek target into the stream's time_base so we can
                 * compare against packet pts. The first few packets after a seek
                 * may be partial or misaligned; discard them to avoid glitches.
                 */
                skip_until_pts = av_rescale_q(target_pts, AV_TIME_BASE_Q, stream->time_base);
                post_seek_flush_pending = true;
            }
            seek_pending = false;
        }

        if (seek_pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            print_status(seek_preview_sec, duration_sec, g_volume.load());
            continue;
        }

        if (is_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            print_status(current_pos_sec, duration_sec, g_volume.load());
            continue;
        }

        if (av_read_frame(fmt_ctx, pkt) < 0) break;
        if (pkt->stream_index != audio_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // Discard packets with pts below skip_until_pts after a seek to avoid
        // sending partial/misaligned DSD data that causes glitches.
        if (skip_until_pts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) {
            if (pkt->pts < skip_until_pts) {
                av_packet_unref(pkt);
                continue;
            }
            skip_until_pts = AV_NOPTS_VALUE;
            post_seek_flush_pending = true;
        }

        size_t needed_frames = 0;

        if (mode == DsdOutputMode::Native) {
            const size_t containerBytes =
                static_cast<size_t>(tinyalsa::bytes_per_frame(native_fmt, 1));

            if (containerBytes == 0) {
                av_packet_unref(pkt);
                continue;
            }

            needed_frames = layout.planar
                ? (layout.block_size / containerBytes)
                : (static_cast<size_t>(pkt->size) / static_cast<size_t>(channels) / containerBytes);
        } else {
            needed_frames = dop_packer.max_output_frames_for(static_cast<size_t>(pkt->size));
        }

        out_buf.resize(needed_frames * out_frame_bytes);

        size_t produced_bytes;
        if (mode == DsdOutputMode::Native) {
            produced_bytes = native_packer.pack(pkt->data, (size_t)pkt->size, out_buf.data(), out_buf.size());
        } else {
            produced_bytes = dop_packer.pack(
                pkt->data, (size_t)pkt->size, reinterpret_cast<int32_t *>(out_buf.data()), out_buf.size() / out_frame_bytes
            ) * out_frame_bytes;
        }

        if (pkt->pts != AV_NOPTS_VALUE) current_pos_sec = pkt->pts * av_q2d(stream->time_base);
        av_packet_unref(pkt);

        if (produced_bytes == 0) continue;
        tinyalsa::size_type total_frames = (tinyalsa::size_type)(produced_bytes / out_frame_bytes);
        tinyalsa::size_type written = 0;

        while (written < total_frames && g_running) {
            int64_t sreq = g_seek_target.load();
            if (sreq != -1) {
                if (sreq == -4) {
                    g_seek_target.store(-1);
                    is_paused = !is_paused;
                    writer.pause(is_paused);
                } else {
                    break;
                }
            }
            if (is_paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            auto avail_res = writer.get_avail();
            if (avail_res.failed()) {
                tinyalsa::pcm_recover(writer, avail_res.error);
                if (writer.get_state().unwrap() == tinyalsa::pcm_state::disconnected) {
                    device_disconnected_local = true;
                    g_running = false;
                    break;
                }
                continue;
            }

            tinyalsa::size_type avail = avail_res.unwrap();
            if (avail == 0) {
                auto state = writer.get_state().unwrap();
                if (state == tinyalsa::pcm_state::disconnected) {
                    device_disconnected_local = true;
                    g_running = false;
                    break;
                } else if (state == tinyalsa::pcm_state::xrun) {
                    writer.prepare();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // After a seek, discard the first buffer worth of audio to ensure
            // clean playback start and avoid residual stale data.
            if (post_seek_flush_pending) {
                post_seek_flush_pending = false;
                writer.prepare();

                if (mode != DsdOutputMode::Native) {
                    dop_packer.reset();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            tinyalsa::size_type to_write = std::min(avail, total_frames - written);
            auto write_res = writer.write_unformatted(out_buf.data() + written * out_frame_bytes, to_write);
            if (write_res.failed()) {
                tinyalsa::pcm_recover(writer, write_res.error);
                if (writer.get_state().unwrap() == tinyalsa::pcm_state::disconnected) {
                    device_disconnected_local = true;
                    g_running = false;
                    break;
                }
            } else {
                written += write_res.unwrap();
            }
        }

        double delay_sec = 0.0;
        auto delay_res = writer.get_delay();
        if (!delay_res.failed()) delay_sec = (double)delay_res.unwrap() / out_rate;
        print_status(std::max(0.0, current_pos_sec - delay_sec), duration_sec, g_volume.load());
    }

    g_running = false;
    if (kb.joinable()) kb.join();

    {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        std::cout << "\r\033[K" << std::flush;
    }

    av_packet_free(&pkt);
    writer.close();
    avformat_close_input(&fmt_ctx);
    cleanup_hw_mixer();

    if (device_disconnected_local || g_device_disconnected) {
        g_device_disconnected = true;
        std::cout << RED << BOLD << "ERROR: Audio device was disconnected\n" << RESET;
        return 2;
    }
    std::cout << GREEN << "Playback finished.\n" << RESET;
    return 0;
}

// ─── Core playback ────────────────────────────────────────────────────────────
static int play(
    const std::string &file_path, const AlsaDevice &dev, bool enable_replaygain, bool force_software_mixer,
    bool allow_native_dsd, bool allow_dop
) {
    tinyalsa::size_type card = dev.card;
    tinyalsa::size_type device = dev.device;
    std::string display_name = dev.name + " [" + dev.hw_id + "]";

    bool hw_mixer_available = false;
    if (!force_software_mixer) {
        hw_mixer_available = init_hw_mixer(card);
    }
    set_volume(g_volume.load());

    // ─── FFmpeg setup ───────────────────────────────────────────────────
    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, file_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << RED << "ERROR: Cannot open file: " << file_path << RESET << "\n";
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << RED << "ERROR: Cannot find stream info.\n" << RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = (int)i;
            break;
        }
    }

    if (audio_idx < 0) {
        std::cerr << RED << "ERROR: No audio stream found.\n" << RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream *stream = fmt_ctx->streams[audio_idx];
    double duration_sec = (stream->duration != AV_NOPTS_VALUE) ? stream->duration * av_q2d(stream->time_base) :
                                                                 (double)fmt_ctx->duration / AV_TIME_BASE;

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << RED << "ERROR: Codec not found.\n" << RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << RED << "ERROR: Cannot open codec.\n" << RESET;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    bool dsd_mode = is_dsd_codec(codec_ctx->codec_id);
    int in_rate = codec_ctx->sample_rate ? codec_ctx->sample_rate : 44100;
    int out_rate = in_rate;
    int out_channels = 2;

    // DoP / native DSD bypass FFmpeg's software DSD-to-PCM decode entirely, so the
    // decision has to happen before any of that setup runs, and control hands off
    // to a dedicated raw playback path if the device can take the bitstream as-is.
    if (dsd_mode && (allow_native_dsd || allow_dop)) {
        tinyalsa::pcm_params probe;
        if (!probe.open(card, device, false).failed()) {
            tinyalsa::size_type dsd_rate = (tinyalsa::size_type)in_rate;
            DsdOutputMode mode = choose_dsd_output_mode(probe, dsd_rate, allow_native_dsd, allow_dop);
            probe.close();

            if (mode != DsdOutputMode::Pcm) {
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt_ctx);
                return play_dsd_raw(file_path, dev, mode);
            }
        }
    }

    // ─── Replaygain (optional) ──────────────────────────────
    float replaygain_mult = 1.0f;
    bool rg_active = false;
    if (enable_replaygain) {
        ReplayGainProcessor rg;
        rg.parseMetadata(fmt_ctx->metadata);
        if (rg.isAvailable()) {
            replaygain_mult = rg.getMultiplier();
            rg_active = true;
        }
    }

    // ─── TinyALSA negotiation ───────────────────────────────────────────
    tinyalsa::pcm_params params;
    if (params.open(card, device, false).failed()) {
        std::cerr << RED << "ERROR: Cannot open PCM params for hw:" << card << "," << device << RESET << "\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    std::vector<tinyalsa::sample_format> supported_fmts;
    params.for_each_supported_format(
        [](tinyalsa::sample_format fmt, void *ud) {
            static_cast<std::vector<tinyalsa::sample_format> *>(ud)->push_back(fmt);
        },
        &supported_fmts
    );

    tinyalsa::sample_format best_fmt = tinyalsa::sample_format::s16_le;
    const tinyalsa::sample_format format_prefs[] = {
        tinyalsa::sample_format::s32_le, tinyalsa::sample_format::s24_le, tinyalsa::sample_format::s16_le
    };

    for (auto pref : format_prefs) {
        if (std::find(supported_fmts.begin(), supported_fmts.end(), pref) != supported_fmts.end()) {
            best_fmt = pref;
            break;
        }
    }

    if (dsd_mode) {
        auto max_rate_r = params.get_max_rate();
        if (!max_rate_r.failed()) {
            out_rate = static_cast<int>(max_rate_r.unwrap());
        }
    }

    if (!params.test_channels(out_channels)) {
        std::cerr << RED << "ERROR: Device does not support " << out_channels << " channels.\n" << RESET;
        params.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (!params.test_rate((unsigned)out_rate)) {
        std::cerr << RED << "ERROR: Device does not support rate " << out_rate << "\n" << RESET;
        params.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    tinyalsa::size_type period_size = 1024;
    tinyalsa::size_type period_count = 4;

    auto min_period = params.get_min_period_size();
    auto max_period = params.get_max_period_size();

    if (!min_period.failed() && !max_period.failed()) {
        tinyalsa::size_type lo = min_period.unwrap();
        tinyalsa::size_type hi = max_period.unwrap();

        // Clamp to device range
        if (period_size < lo) period_size = lo;
        if (period_size > hi) period_size = hi;

        // Round down to a power of two within range (most drivers prefer this)
        tinyalsa::size_type p = 1;
        while (p * 2 <= period_size) p *= 2;
        if (p >= lo) period_size = p;
    }

    params.close();

    // ─── Resampler & Flags Check ───────────────────────────────────────
    Resampler resampler;
    AVChannelLayout in_layout;
    if (codec_ctx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&in_layout, &codec_ctx->ch_layout);
    } else {
        av_channel_layout_default(&in_layout, 1);
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels);

    if (!resampler.init(codec_ctx->sample_rate, in_layout, codec_ctx->sample_fmt, out_rate, out_layout, AV_SAMPLE_FMT_S32)) {
        std::cerr << RED << "ERROR: Resampler init failed.\n" << RESET;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    // Determine Processing Flags
    std::vector<std::string> flags;
    if (in_rate != out_rate || codec_ctx->ch_layout.nb_channels != out_channels) flags.push_back("Resampling");
    if (rg_active) flags.push_back("ReplayGain");
    if (!hw_mixer_available) flags.push_back("SoftwareMixer");
    if (dsd_mode) flags.push_back("DSD2PCM");

    std::string flags_str;
    if (flags.empty()) {
        flags_str = "BitPerfect";
    } else {
        for (size_t i = 0; i < flags.size(); ++i) {
            flags_str += flags[i] + (i < flags.size() - 1 ? " " : "");
        }
    }

    // ─── Open ALSA writer ───────────────────────────────────────────────
    tinyalsa::interleaved_pcm_writer writer;
    if (writer.open(card, device, false).failed()) {
        std::cerr << RED << "ERROR: Cannot open PCM device hw:" << card << "," << device << RESET << "\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    tinyalsa::pcm_config config;
    config.channels = out_channels;
    config.rate = (unsigned)out_rate;
    config.format = best_fmt;
    config.period_size = period_size;
    config.period_count = period_count;

    if (writer.setup(config).failed() || writer.prepare().failed()) {
        std::cerr << RED << "ERROR: PCM configuration failed.\n" << RESET;
        writer.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    // ─── Print header ───────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(g_print_mutex);

        // clang-format off
        std::cout << "\n"
                  << BOLD << CYAN
                  << "╔═════════════════════════════════════════════════════╗\n"
                  << "║                       Leticia                       ║\n"
                  << "╚═════════════════════════════════════════════════════╝\n"
                  << RESET << GREEN << "  File   : " << WHITE << file_path << "\n"
                  << RESET << GREEN << "  Device : " << WHITE << display_name << "\n"
                  << RESET << GREEN << "  Mixer  : " << WHITE << (hw_mixer_available ? "Hardware" : "Software") << "\n"
                  << RESET << GREEN << "  Rate   : " << WHITE << out_rate << " Hz  " << out_channels << " ch  "
                  << tinyalsa::to_string(best_fmt) << "\n"
                  << RESET << GREEN << "  Flags  : " << WHITE << flags_str << "\n\n"
                  << RESET << YELLOW << "  ← / → : Seek   ↑ / ↓ : Volume   SPACE : Pause   q : Quit\n"
                  << RESET << "\n";
        // clang-format on
    }

    // ─── Start watcher threads ───────────────────────────────────────────
    std::thread watcher(watch_mixer_thread);
    std::thread kb(keyboard_thread);
    watcher.detach();

    // ─── Main decode loop ───────────────────────────────────────────────
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const bool is_32bit = (best_fmt == tinyalsa::sample_format::s32_le);
    std::vector<uint8_t> pcm_buf;

    double current_pos_sec = 0.0;
    bool is_paused = false;

    // Pre-roll discard threshold: after a seek, frames with pts below this
    // value are decoded but not sent to ALSA, allowing the Opus/MDCT decoder
    // to warm up its internal state before producing audible output.
    int64_t skip_until_pts = AV_NOPTS_VALUE;

    constexpr auto kSeekDebounce = std::chrono::milliseconds(250);
    constexpr double kSeekStepSec = 5.0;
    bool seek_pending = false;
    double seek_preview_sec = 0.0;
    std::chrono::steady_clock::time_point last_seek_key_time{};

    while (g_running) {
        int64_t seek_req = g_seek_target.exchange(-1);
        if (seek_req == -4) {
            is_paused = !is_paused;
            writer.pause(is_paused);
        } else if (seek_req == -2 || seek_req == -3) {
            double offset = (seek_req == -2) ? kSeekStepSec : -kSeekStepSec;
            double base = seek_pending ? seek_preview_sec : current_pos_sec;
            seek_preview_sec = std::max(0.0, base + offset);
            seek_pending = true;
            last_seek_key_time = std::chrono::steady_clock::now();
        }

        // Commit the real seek only after the debounce window has elapsed
        // with no further seek key presses.
        if (seek_pending && (std::chrono::steady_clock::now() - last_seek_key_time) >= kSeekDebounce) {
            double target_sec = seek_preview_sec;
            int64_t target_pts = static_cast<int64_t>(target_sec * AV_TIME_BASE);

            if (avformat_seek_file(fmt_ctx, -1, INT64_MIN, target_pts, INT64_MAX, 0) >= 0) {
                avcodec_flush_buffers(codec_ctx);
                writer.prepare(); // Drop stale audio in ALSA hardware buffer
                resampler.init(codec_ctx->sample_rate, in_layout, codec_ctx->sample_fmt, out_rate, out_layout, AV_SAMPLE_FMT_S32);
                current_pos_sec = target_sec;

                /*
                 * Convert the seek target from AV_TIME_BASE into the stream's
                 * own time-base so we can compare directly against frame->pts.
                 * Frames decoded before this threshold are pre-roll warmup and
                 * must be discarded silently (required by Opus / MDCT codecs).
                 */
                skip_until_pts = av_rescale_q(target_pts, AV_TIME_BASE_Q, stream->time_base);
            }
            seek_pending = false;
        }

        if (seek_pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            print_status(seek_preview_sec, duration_sec, g_volume.load());
            continue;
        }

        if (is_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            double delay_sec = 0.0;
            auto delay_res = writer.get_delay();
            if (!delay_res.failed()) delay_sec = (double)delay_res.unwrap() / out_rate;
            print_status(std::max(0.0, current_pos_sec - delay_sec), duration_sec, g_volume.load());
            continue;
        }

        if (av_read_frame(fmt_ctx, pkt) < 0) break;

        if (pkt->stream_index != audio_idx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        av_packet_unref(pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            if (!g_running) break;

            /*
             * Opus / MDCT pre-roll discard
             *
             * After a seek, FFmpeg lands on the nearest OGG page boundary
             * *before* the requested position. The decoder has no prior state
             * at that point, so the first few frames are corrupt. We silently
             * decode and discard them until we reach the intended target PTS,
             * at which point the decoder's overlap buffers are properly filled.
             */
            if (skip_until_pts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
                if (frame->pts < skip_until_pts) {
                    av_frame_unref(frame);
                    continue;
                }
                skip_until_pts = AV_NOPTS_VALUE;
            }

            int max_out = resampler.getDelay(out_rate) + frame->nb_samples + 64;
            size_t bytes_per_sample = is_32bit ? sizeof(int32_t) : sizeof(int16_t);
            size_t frame_bytes = out_channels * bytes_per_sample;
            pcm_buf.resize(max_out * frame_bytes);

            uint8_t *out_ptr = pcm_buf.data();
            int converted = resampler.convert((const uint8_t **)frame->data, frame->nb_samples, &out_ptr, max_out);
            if (converted <= 0) {
                av_frame_unref(frame);
                continue;
            }

            float effective_vol = SoftwareMixer::getVolumeFactor(g_volume.load()) * replaygain_mult;
            float sw_multiplier = g_hw_mixer_active ? replaygain_mult : effective_vol;

            if (std::abs(sw_multiplier - 1.0f) > 0.001f) {
                if (is_32bit) {
                    SoftwareMixer::applyVolume(
                        reinterpret_cast<int32_t *>(pcm_buf.data()), converted * out_channels, sw_multiplier
                    );
                } else {
                    SoftwareMixer::applyVolume(
                        reinterpret_cast<int16_t *>(pcm_buf.data()), converted * out_channels, sw_multiplier
                    );
                }
            }

            tinyalsa::size_type written = 0;
            while (written < static_cast<tinyalsa::size_type>(converted) && g_running) {
                int64_t sreq = g_seek_target.load();
                if (sreq != -1) {
                    if (sreq == -4) {
                        g_seek_target.store(-1);
                        is_paused = !is_paused;
                        writer.pause(is_paused);
                    } else {
                        break; // Seek triggered, escape write loop
                    }
                }

                if (is_paused) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                auto avail_res = writer.get_avail();
                if (avail_res.failed()) {
                    tinyalsa::pcm_recover(writer, avail_res.error);
                    if (writer.get_state().unwrap() == tinyalsa::pcm_state::disconnected) {
                        g_device_disconnected = true;
                        g_running = false;
                        break;
                    }
                    continue;
                }

                tinyalsa::size_type avail = avail_res.unwrap();
                if (avail == 0) {
                    auto state = writer.get_state().unwrap();
                    if (state == tinyalsa::pcm_state::disconnected) {
                        g_device_disconnected = true;
                        g_running = false;
                        break;
                    } else if (state == tinyalsa::pcm_state::xrun) {
                        writer.prepare();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                tinyalsa::size_type to_write = std::min(avail, static_cast<tinyalsa::size_type>(converted - written));
                auto write_res = writer.write_unformatted(pcm_buf.data() + written * frame_bytes, to_write);

                if (write_res.failed()) {
                    tinyalsa::pcm_recover(writer, write_res.error);
                    if (writer.get_state().unwrap() == tinyalsa::pcm_state::disconnected) {
                        g_device_disconnected = true;
                        g_running = false;
                        break;
                    }
                } else {
                    written += write_res.unwrap();
                }
            }

            int64_t sreq = g_seek_target.load();
            if (sreq != -1 && sreq != -4) {
                av_frame_unref(frame);
                break;
            }

            if (frame->pts != AV_NOPTS_VALUE) current_pos_sec = frame->pts * av_q2d(stream->time_base);

            double delay_sec = 0.0;
            auto delay_res = writer.get_delay();
            if (!delay_res.failed()) {
                delay_sec = (double)delay_res.unwrap() / out_rate;
            }
            print_status(std::max(0.0, current_pos_sec - delay_sec), duration_sec, g_volume.load());

            av_frame_unref(frame);
        }
    }

    // ─── Cleanup ──────────────────────────────────────────────────────────
    g_running = false;
    if (kb.joinable()) kb.join();

    {
        // Clean up the progress bar line
        std::lock_guard<std::mutex> lk(g_print_mutex);
        std::cout << "\r\033[K" << std::flush;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    writer.close();
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    cleanup_hw_mixer();

    if (g_device_disconnected) {
        std::cout << RED << BOLD << "ERROR: Audio device was disconnected\n" << RESET;
        return 2;
    }

    std::cout << GREEN << "Playback finished.\n" << RESET;
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (geteuid() != 0) {
        std::cerr << RED << BOLD << "ERROR: Must run as root.\n" << RESET;
        return 1;
    }

    if (argc < 2) {
        std::cerr << YELLOW
                  << "Usage: " << argv[0]
                  << " <file> [hw:card,device] [--replaygain] [--software-mixer] [--dop] [--native-dsd] [--pcm-only]\n"
                  << RESET;
        return 1;
    }

    std::string file_path = argv[1];
    bool enable_replaygain = false;
    bool force_software_mixer = false;
    bool force_dop = false;
    bool force_native_dsd = false;
    bool pcm_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--replaygain") enable_replaygain = true;
        if (std::string(argv[i]) == "--software-mixer") force_software_mixer = true;
        if (std::string(argv[i]) == "--dop") force_dop = true;
        if (std::string(argv[i]) == "--native-dsd") force_native_dsd = true;
        if (std::string(argv[i]) == "--pcm-only") pcm_only = true;
    }

    // Default: try native DSD first, fall back to DoP, then to software PCM conversion.
    // --dop / --native-dsd narrow that to a single strategy; --pcm-only disables both.
    bool allow_native_dsd = !pcm_only && !force_dop;
    bool allow_dop = !pcm_only && !force_native_dsd;

    AlsaDevice dev;
    if (argc >= 3 && std::string(argv[2]).find("hw:") == 0) {
        if (!parse_hw_id(argv[2], dev.card, dev.device)) return 1;

        dev.hw_id = "hw:" + std::to_string(dev.card) + "," + std::to_string(dev.device);
        dev.name = get_card_name(dev.card);
    } else {
        std::vector<AlsaDevice> usb_devices = enumerate_usb_playback_devices();
        if (usb_devices.empty()) {
            if (has_usb_audio_cards()) {
                std::cerr << RED
                          << "ERROR: USB DAC detected, but it is currently busy (likely taken by Android Audio HAL or another "
                             "process).\n"
                          << RESET << YELLOW << "TIP: Try stopping all media playback, reconnect the DAC, and try again.\n"
                          << RESET;
            } else {
                std::cerr << RED << "ERROR: No external DAC detected.\n" << RESET;
            }

            return 1;
        }

        if (usb_devices.size() == 1) {
            dev = usb_devices[0];
        } else {
            dev = prompt_device_selection(usb_devices);
        }
    }

    if (!dev.is_valid()) {
        std::cerr << RED << "ERROR: Invalid or no device selected.\n" << RESET;
        return 1;
    }

    g_volume.store(load_volume());
    RawTerminal raw;
    raw.enable();

    return play(file_path, dev, enable_replaygain, force_software_mixer, allow_native_dsd, allow_dop);
}
