/**
 * Alsa.hpp - ALSA device management and enumeration
 */

#pragma once

#include "TinyAlsa.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @struct AlsaDevice
 * @brief Represents an enumerated ALSA playback device
 */
struct AlsaDevice {
    tinyalsa::size_type card = tinyalsa::invalid_card();     ///< Card index
    tinyalsa::size_type device = tinyalsa::invalid_device(); ///< Device index on card
    std::string hw_id;                                       ///< Hardware ID format: "hw:card,device"
    std::string name;                                        ///< Human-readable device name (card + device)

    bool is_valid() const noexcept {
        return card != tinyalsa::invalid_card();
    }
};

/**
 * @brief Get ALSA card name from /proc/asound
 * @param card Card index
 * @return Card name or "Unknown Card"
 */
std::string get_card_name(tinyalsa::size_type card);

/**
 * @brief Enumerate all ALSA playback devices
 * @return Vector of AlsaDevice structures (all devices)
 */
std::vector<AlsaDevice> enumerate_pcm_devices();

/**
 * @brief Enumerate USB/external ALSA playback devices (card index > 0)
 * @return Vector of AlsaDevice structures (USB devices only)
 */
std::vector<AlsaDevice> enumerate_usb_playback_devices();

/**
 * @brief Interactive device selection menu
 * @param devices Vector of available devices
 * @return Selected AlsaDevice, or empty device if cancelled
 */
AlsaDevice prompt_device_selection(const std::vector<AlsaDevice> &devices);

/**
 * @brief Check whether any USB audio card exists in procfs,
 * even if the PCM device is currently held by another process.
 * @return true if at least one USB audio card (card index > 0) is found
 */
bool has_usb_audio_cards();

/**
 * @brief Parse hardware ID string (e.g., "hw:1,0" or "hw:1")
 * @param s Input string
 * @param card Output: card index
 * @param device Output: device index
 * @return true on success, false on parse error
 */
bool parse_hw_id(const std::string &s, tinyalsa::size_type &card, tinyalsa::size_type &device);
