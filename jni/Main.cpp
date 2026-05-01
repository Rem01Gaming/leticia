#include "Alsa.hpp"
#include "AnsiColors.hpp"
#include "Replaygain.hpp"
#include "Resampler.hpp"
#include "SoftwareMixer.hpp"
#include "TinyAlsa.hpp"

#include <algorithm>
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

// ─── Core playback ────────────────────────────────────────────────────────────
static int play(const std::string &file_path, const AlsaDevice &dev, bool enable_replaygain) {
    tinyalsa::size_type card = dev.card;
    tinyalsa::size_type device = dev.device;
    std::string display_name = dev.name + " [" + dev.hw_id + "]";

    bool hw_mixer_available = init_hw_mixer(card);
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

    while (g_running) {
        int64_t seek_req = g_seek_target.exchange(-1);
        if (seek_req == -4) {
            is_paused = !is_paused;
            writer.pause(is_paused);
        } else if (seek_req == -2 || seek_req == -3) {
            double offset = (seek_req == -2) ? 5.0 : -5.0;
            double target_sec = std::max(0.0, current_pos_sec + offset);
            int64_t target_pts = static_cast<int64_t>(target_sec / av_q2d(stream->time_base));

            if (av_seek_frame(fmt_ctx, audio_idx, target_pts, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(codec_ctx);
                resampler.init(codec_ctx->sample_rate, in_layout, codec_ctx->sample_fmt, out_rate, out_layout, AV_SAMPLE_FMT_S32);
                current_pos_sec = target_sec;
            }
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

            float effective_vol = g_volume.load() * replaygain_mult;
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
        std::cerr << YELLOW << "Usage: " << argv[0] << " <file> [hw:card,device] [--replaygain]\n" << RESET;
        return 1;
    }

    std::string file_path = argv[1];
    bool enable_replaygain = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--replaygain") enable_replaygain = true;
    }

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

    return play(file_path, dev, enable_replaygain);
}
