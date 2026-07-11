#include "telnet_settings.h"
#include <cstring>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace {
constexpr uint32_t kMagic = 0x4D413257; // "MA2W" / v0.2.3 settings layout
// Dedicated sector. Kept away from BTstack NVM and original DS5 config area.
constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - 7u * FLASH_SECTOR_SIZE;
Ma2TelnetSettings g_settings{};

bool sane(const Ma2TelnetSettings& s) {
    if (s.magic != kMagic) return false;
    if (s.pico_ip[0] == 0 || s.netmask[0] == 0 || s.ma2_port == 0) return false;
    if (s.deadzone_percent > 30) return false;
    if (s.speed1_percent < 10 || s.speed1_percent > 80) return false;
    if (s.speed2_percent < s.speed1_percent || s.speed2_percent > 95) return false;
    for (int i = 0; i < 3; ++i) {
        if (s.step_x10[i] < 1 || s.step_x10[i] > 200) return false;
        if (s.rate_ms[i] < 20 || s.rate_ms[i] > 500) return false;
    }
    return true;
}
}

Ma2TelnetSettings telnet_settings_default() {
    Ma2TelnetSettings s{};
    s.magic = kMagic;
    s.pico_ip[0] = 192; s.pico_ip[1] = 168; s.pico_ip[2] = 7; s.pico_ip[3] = 2;
    s.netmask[0] = 255; s.netmask[1] = 255; s.netmask[2] = 255; s.netmask[3] = 0;
    s.gateway[0] = 192; s.gateway[1] = 168; s.gateway[2] = 7; s.gateway[3] = 1;
    s.ma2_ip[0] = 192; s.ma2_ip[1] = 168; s.ma2_ip[2] = 7; s.ma2_ip[3] = 1;
    s.ma2_port = 30000;
    s.deadzone_percent = 5;
    s.speed1_percent = 35;
    s.speed2_percent = 70;
    s.step_x10[0] = 10;   // 1.0
    s.step_x10[1] = 30;   // 3.0
    s.step_x10[2] = 100;  // 10.0
    s.rate_ms[0] = 120;
    s.rate_ms[1] = 70;
    s.rate_ms[2] = 40;
    std::strncpy(s.username, "Administrator", sizeof(s.username) - 1);
    std::strncpy(s.password, "admin", sizeof(s.password) - 1);
    return s;
}

void telnet_settings_load() {
    const auto* flash = reinterpret_cast<const Ma2TelnetSettings*>(XIP_BASE + kFlashOffset);
    if (sane(*flash)) std::memcpy(&g_settings, flash, sizeof(g_settings));
    else g_settings = telnet_settings_default();
}

void telnet_settings_save() {
    g_settings.magic = kMagic;
    uint8_t page[FLASH_PAGE_SIZE]{};
    std::memcpy(page, &g_settings, sizeof(g_settings));
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
    flash_range_program(kFlashOffset, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

const Ma2TelnetSettings& telnet_settings_get() { return g_settings; }
void telnet_settings_set(const Ma2TelnetSettings& s) { g_settings = s; g_settings.magic = kMagic; }
