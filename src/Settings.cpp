/**
 * Persistent settings – EEPROM (flash-emulated on RP2040).
 * Writes only happen when the user explicitly triggers a save.
 */

#include "Settings.h"
#include <EEPROM.h>
#include <string.h>  // memcmp

// "ATFS" in little-endian
static const uint32_t SETTINGS_MAGIC   = 0x53465441;
static const uint8_t  SETTINGS_VERSION = 1;

// EEPROM size we reserve (must be >= sizeof(SettingsData))
static const size_t EEPROM_SIZE = 256;

static SettingsData  _committed;     // last known flash contents (for change detection)
static bool          _hasCommitted = false;

namespace Settings {

void begin() {
    EEPROM.begin(EEPROM_SIZE);
}

bool load(SettingsData& data) {
    EEPROM.get(0, data);

    if (data.magic != SETTINGS_MAGIC || data.version != SETTINGS_VERSION) {
        return false;  // No valid settings found
    }

    // Basic sanity checks
    if (data.duration_sec == 0 || data.duration_sec > 3600) return false;
    if (data.threshold_count > 10)                          return false;
    if (data.orientation != 0 && data.orientation != 180)   return false;

    // Cache so subsequent save() can detect changes
    _committed    = data;
    _hasCommitted = true;

    return true;
}

bool save(const SettingsData& data) {
    SettingsData out = data;
    out.magic   = SETTINGS_MAGIC;
    out.version = SETTINGS_VERSION;

    // Skip write if nothing actually changed
    if (_hasCommitted && memcmp(&out, &_committed, sizeof(SettingsData)) == 0) {
        return false;
    }

    EEPROM.put(0, out);
    EEPROM.commit();
    _committed    = out;
    _hasCommitted = true;
    return true;
}

} // namespace Settings
