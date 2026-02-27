/**
 * Persistent settings storage using RP2040 flash-emulated EEPROM.
 *
 * Settings are written to flash after a short cooldown (SAVE_DELAY_MS)
 * following the last change, to avoid excessive flash wear from rapid
 * slider/knob adjustments on the web UI.
 */

#pragma once

#include <stdint.h>

/// On-disk settings layout.  Bump SETTINGS_VERSION when fields change.
struct SettingsData {
    uint32_t magic;           // Must equal SETTINGS_MAGIC for valid data
    uint8_t  version;         // Struct version (SETTINGS_VERSION)

    // Timer
    uint16_t duration_sec;    // Duration in seconds (1-3600)

    // Display
    uint8_t  font_id;         // Font ID (see getFontById)
    int8_t   spacing;         // Letter spacing in pixels
    uint8_t  brightness;      // 0-255
    uint16_t orientation;     // 0 or 180

    // Default color (when no threshold matches)
    uint8_t  default_r;
    uint8_t  default_g;
    uint8_t  default_b;

    // Color thresholds
    uint8_t  threshold_count; // 0-10
    struct Threshold {
        uint16_t seconds;
        uint8_t  r, g, b;
    } thresholds[10];
};

namespace Settings {
    /// Call once in setup() before loading settings.
    void begin();

    /// Try to load settings from flash.
    /// @return true if valid settings were found and `data` was populated.
    bool load(SettingsData& data);

    /// Write settings to flash immediately.  Skips the write if data
    /// matches what is already stored (no flash wear in that case).
    /// @return true if flash was actually written, false if skipped (unchanged).
    bool save(const SettingsData& data);
}
