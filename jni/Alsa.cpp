/**
 * Alsa.cpp - ALSA device management and enumeration implementation
 */

#include "Alsa.hpp"
#include "AnsiColors.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <unistd.h>

// ─── Read card name from procfs ─────────────────────────────────────────────
std::string get_card_name(tinyalsa::size_type card) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/asound/card%lu/id", (unsigned long)card);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return "Unknown Card";

    char buf[64] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);

    if (n > 0) {
        if (buf[n - 1] == '\n') buf[n - 1] = '\0';
        return buf;
    }
    return "Unknown Card";
}

// ─── ALSA device enumeration ──────────────────────────────────────────────────
std::vector<AlsaDevice> enumerate_pcm_devices() {
    std::vector<AlsaDevice> devs;
    tinyalsa::pcm_list list;

    for (tinyalsa::size_type i = 0; i < list.size(); ++i) {
        const auto &info = list[i];

        // Skip capture devices
        if (info.is_capture) continue;

        AlsaDevice d;
        d.card = info.card;
        d.device = info.device;
        d.hw_id = "hw:" + std::to_string(info.card) + "," + std::to_string(info.device);

        std::string card_name = get_card_name(info.card);
        std::string pcm_name = info.name[0] ? info.name : (info.id[0] ? info.id : "Unknown");
        d.name = card_name + " / " + pcm_name;

        devs.push_back(d);
    }
    return devs;
}

std::vector<AlsaDevice> enumerate_usb_playback_devices() {
    auto all_devices = enumerate_pcm_devices();
    std::vector<AlsaDevice> usb_devices;

    // Filter for USB/external devices (card index > 0)
    for (const auto &dev : all_devices) {
        if (dev.card > 0) { // card 0 is typically built-in
            usb_devices.push_back(dev);
        }
    }

    return usb_devices;
}

// ─── Device selection menu ────────────────────────────────────────────────────
AlsaDevice prompt_device_selection(const std::vector<AlsaDevice> &devices) {
    if (devices.empty()) {
        std::cerr << RED << "ERROR: No devices available for selection.\n" << RESET;
        return {};
    }

    std::cout << "\n" << BOLD << CYAN << "┌─ Available Audio Devices ──────────────────────────────┐\n" << RESET;

    for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << CYAN << "│ " << RESET << BOLD << WHITE << "[" << i << "] " << RESET << GREEN << std::left << std::setw(10)
                  << devices[i].hw_id << RESET << "  " << DIM << devices[i].name << RESET << "\n";
    }

    std::cout << CYAN << "└────────────────────────────────────────────────────────┘\n" << RESET;
    std::cout << YELLOW << "Select device [0-" << devices.size() - 1 << "]: " << RESET;

    std::string input;
    std::getline(std::cin, input);

    size_t choice = 0;
    try {
        choice = std::stoul(input);
    } catch (...) {
        std::cerr << RED << "\nInvalid input: not a number.\n" << RESET;
        return {};
    }

    if (choice >= devices.size()) {
        std::cerr << RED << "\nInvalid selection: " << choice << " is out of range.\n" << RESET;
        return {};
    }

    return devices[choice];
}

bool has_usb_audio_cards() {
    for (int n = 1; n < 32; ++n) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/asound/card%d/usbbus", n);
        if (::access(path, F_OK) == 0) return true;
    }
    return false;
}

// ─── Hardware ID parser ───────────────────────────────────────────────────────
bool parse_hw_id(const std::string &s, tinyalsa::size_type &card, tinyalsa::size_type &device) {
    unsigned int c = 0, d = 0;

    if (sscanf(s.c_str(), "hw:%u,%u", &c, &d) == 2) {
        card = c;
        device = d;
        return true;
    }

    if (sscanf(s.c_str(), "hw:%u", &c) == 1) {
        card = c;
        device = 0;
        return true;
    }

    return false;
}
