//
// Created by awalol on 2026/5/4.
//

#include "config.h"

#include <cmath>
#include <cstring>

#include "utils.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "audio.h"

constexpr uint32_t CONFIG_MAGIC = 0x66ccff00;
constexpr uint16_t CONFIG_VERSION = 8;
// Persistent app data must not live in the last two flash sectors: BTstack
// uses them as its NVM/link-key flash banks on Pico/Pico W ports. The old
// Ohad builds stored Config in the very last sector, which explains why
// Remap (-3) survived firmware updates but Settings (-1) could reset.
// New layout: Remap=-3, Config=-4, Slots=-5. Legacy Config (-1) is read
// once for migration if it is still valid, but is never erased/programmed.
constexpr uint32_t CONFIG_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE;
constexpr uint32_t LEGACY_CONFIG_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
static Config config{};
bool is_dse = false;

// fixed65am/1.0.4 AudioRouteFix: Flash erase/program stalls XIP and can
// momentarily break active USB audio/haptics. Defer normal config saves while
// the host has real recent USB audio activity, not merely a stale alt setting
// left behind by a browser/test page. Poweroff still forces a sync save.
extern bool controller_poweroff_is_pending();
static volatile bool g_config_save_deferred = false;
static uint32_t g_config_audio_quiet_since_us = 0;
static bool config_write_flash_now();

static bool config_flash_audio_busy() {
    return audio_usb_active();
}

static bool config_flash_audio_quiet_for(uint32_t quiet_us) {
    const uint32_t now = time_us_32();
    if (config_flash_audio_busy()) {
        g_config_audio_quiet_since_us = now;
        return false;
    }
    if (g_config_audio_quiet_since_us == 0) {
        g_config_audio_quiet_since_us = now;
        return false;
    }
    return (uint32_t)(now - g_config_audio_quiet_since_us) >= quiet_us;
}

// 编译期保护
// 判断Config结构体是否能放进flash 256bytes
static_assert(sizeof(Config) <= FLASH_PAGE_SIZE);
// 配置区起始地址必须按 flash sector 对齐。
static_assert(CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);
static_assert(LEGACY_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

uint32_t calc_config_crc(const Config &con) {
    return crc32(reinterpret_cast<const uint8_t *>(&con.body), sizeof(Config_body));
}

static uint32_t calc_config_crc_len(const Config &con, uint16_t len) {
    if (len > sizeof(Config_body)) len = sizeof(Config_body);
    return crc32(reinterpret_cast<const uint8_t *>(&con.body), len);
}

const Config *flash_config() {
    return reinterpret_cast<const Config *>(XIP_BASE + CONFIG_FLASH_OFFSET);
}

static const Config *legacy_flash_config() {
    return reinterpret_cast<const Config *>(XIP_BASE + LEGACY_CONFIG_FLASH_OFFSET);
}

static bool config_record_looks_valid(const Config &candidate) {
    if (candidate.magic != CONFIG_MAGIC) return false;
    if (candidate.size == 0 || candidate.size > sizeof(Config_body)) return false;
    // Accept both current and older schema versions. Older builds stored a
    // shorter Config_body and calculated CRC only over that saved length.
    return calc_config_crc_len(candidate, candidate.size) == candidate.crc32;
}

void config_valid() {
    // valid config and set default value
    if (config.magic != CONFIG_MAGIC) {
        config.magic = CONFIG_MAGIC;
        printf("[Config] Config Magic Header is invalid\n");
    }
    if (config.version != CONFIG_VERSION) {
        config.version = CONFIG_VERSION;
        printf("[Config] Config Version is invalid\n");
    }
    if (config.size != sizeof(Config_body)) {
        config.size = sizeof(Config_body);
        printf("[Config] Config Body size is invalid\n");
    }
    auto body = &config.body;
    const uint8_t previous_body_config_version = body->config_version;
    if (std::isnan(body->haptics_gain) || body->haptics_gain < 1.0f || body->haptics_gain > 2.0f) {
        body->haptics_gain = 1.0f;
        printf("[Config] Haptics Gain value is invalid\n");
    }
    if (std::isnan(body->speaker_volume) || body->speaker_volume < -100 || body->speaker_volume > 0) {
        body->speaker_volume = 0;  // OLED Edition: 0 dB default (unity); -100 would be silent
        printf("[Config] Speaker Volume is invalid, defaulting to 0 dB\n");
    }
    // fixed65u: Idle menu choices are Off / 1 / 2 / 3 / 5 / 10 / 20 / 30 min.
    // Off is represented by disable_inactive_disconnect=1; inactive_time keeps
    // the last selected minute value for when Idle is re-enabled.
    if (body->inactive_time != 1 && body->inactive_time != 2 &&
        body->inactive_time != 3 && body->inactive_time != 5 &&
        body->inactive_time != 10 && body->inactive_time != 20 &&
        body->inactive_time != 30) {
        body->inactive_time = 5;
        printf("[Config] Idle time invalid, defaulting to 5 min\n");
    }
    if (body->disable_inactive_disconnect > 1) {
        body->disable_inactive_disconnect = 0;
        printf("[Config] disable_auto_disconnect is invalid\n");
    }
    if (body->disable_pico_led > 1) {
        body->disable_pico_led = 1;
        printf("[Config] disable_pico_led is invalid, defaulting to off\n");
    }
    // Pico LED is no longer user-facing in the OLED menu. Keep it off by
    // default/fixed so the board LED does not distract during normal use.
    // Critical battery warnings can still override this temporarily.
    body->disable_pico_led = 1;
    if (body->polling_rate_mode > 2) {
        body->polling_rate_mode = 2; // Ohad custom default: real-time polling
        printf("[Config] polling_rate_mode is invalid, defaulting to RT\n");
    }
    if (body->audio_buffer_length < 16 || body->audio_buffer_length > 128) {
        body->audio_buffer_length = 64;
        printf("[Config] haptics_buffer_length is invalid\n");
    }
    if (body->controller_mode > 2) {
        body->controller_mode = 2;
        printf("[Config] controller_mode is invalid\n");
    }
    if (body->current_slot >= 4) {
        body->current_slot = 0;
        printf("[Config] current_slot is invalid\n");
    }
    if (body->auto_haptics_enable > 3) {
        body->auto_haptics_enable = 0; // Ohad default: Off
        printf("[Config] auto_haptics_enable invalid, defaulting to 0 (Off)\n");
    }
    if (body->auto_haptics_gain > 200) {
        body->auto_haptics_gain = 100;
        printf("[Config] auto_haptics_gain invalid, defaulting to 100\n");
    }
    if (body->auto_haptics_lowpass > 3) {
        body->auto_haptics_lowpass = 1; // 160 Hz
        printf("[Config] auto_haptics_lowpass invalid, defaulting to 1 (160 Hz)\n");
    }
    if (body->lightbar_mode != 8 && body->lightbar_mode != 9) { // fixed65: only HOST/BATT
        body->lightbar_mode = 9; // Ohad default: BATT
        printf("[Config] lightbar_mode invalid, defaulting to 9 (BATT)\n");
    }
    // lb_fav_{r,g,b} need no validation — any 0..255 is a legal color, and an
    // erased flash sector (0xFF) yields 4 white favorites, a usable default.
    if (body->screen_dim_timeout > 250) { // 0xFF erased / out of range → default
        body->screen_dim_timeout = 2;     // mirrors the original 2-min dim tier
        printf("[Config] screen_dim_timeout invalid, defaulting to 2 min\n");
    }
    if (body->screen_off_timeout > 250) {
        body->screen_off_timeout = 15;    // mirrors the original 15-min off tier
        printf("[Config] screen_off_timeout invalid, defaulting to 15 min\n");
    }
    if (body->bt_mic_enable > 1) {        // 0xFF erased / upgrade → default ON
        body->bt_mic_enable = 1;
        printf("[Config] bt_mic_enable invalid, defaulting to 1 (on)\n");
    }
    if (body->screen_brightness > 9) {    // fixed65b: 10 OLED brightness entries (0=100%, 9=10%)
        body->screen_brightness = 5;      // Ohad default: 50% brightness
        printf("[Config] screen_brightness invalid, defaulting to 5 (50%%)\n");
    }
    if (body->controller_wakes_display > 1) { // 0xFF erased / upgrade → default ON
        body->controller_wakes_display = 1;
        printf("[Config] controller_wakes_display invalid, defaulting to 1 (on)\n");
    }
    if (body->remap_enable > 1) { // 0xFF erased / upgrade → Ohad default OFF; identity table remains passthrough
        body->remap_enable = 0;
        printf("[Config] remap_enable invalid, defaulting to 0 (off)\n");
    }
    if (body->power_combo_enable > 1) { // 0xFF erased / upgrade → Ohad default ON
        body->power_combo_enable = 1;
        printf("[Config] power_combo_enable invalid, defaulting to 1 (on)\n");
    }
    if (body->screen_rotation > 1) { // 0xFF erased / upgrade → default normal orientation
        body->screen_rotation = 0;
        printf("[Config] screen_rotation invalid, defaulting to 0 (normal)\n");
    }
    // fixed65ak: BT connect guard and OUT burst guard are removed from the OLED
    // menu and kept permanently off. Poweroff still uses the internal guard.
    body->bt_connect_guard_100ms = 0;
    body->out_burst_guard_100ms = 0;
    if (body->mic_gain_db_plus24 > 36) {
        body->mic_gain_db_plus24 = 24;
        printf("[Config] mic_gain_db_plus24 invalid, defaulting to 0 dB reference\n");
    }
    if (body->keep_awake_on_audio > 1) { // 0xFF erased / upgrade → Ohad default ON
        body->keep_awake_on_audio = 1;
        printf("[Config] keep_awake_on_audio invalid, defaulting to 1 (on)\n");
    }
    if (body->ui_language > 1) {
        body->ui_language = 0;
        printf("[Config] ui_language invalid, defaulting to English\n");
    }
    if (body->usb_mode > 1) {
        body->usb_mode = 0;
        printf("[Config] usb_mode invalid, defaulting to normal HID+Audio\n");
    }
    if (body->config_version != CONFIG_VERSION) {
        // DS5Dongle by Ohad 1.0.5 PersistentSettings:
        // Firmware updates must not wipe the user's OLED Settings every time the
        // config schema number changes. Preserve every valid field already read
        // from flash and only initialize fields that were added in newer schemas.
        // A fully-erased sector still has config_version=0xFF and is treated as
        // first boot / factory defaults.
        const bool erased_or_unusable_version =
            (previous_body_config_version == 0x00 || previous_body_config_version == 0xFF);

        if (!erased_or_unusable_version) {
            if (previous_body_config_version <= 4) {
                // DS5Dongle by Ohad 1.0.0 Stable:
                // Re-reference Mic Gain so old -20 dB becomes new 0 dB.
                const int old_db = (int)body->mic_gain_db_plus24 - 24;
                int new_ref_db = old_db + 20;
                if (new_ref_db < -24) new_ref_db = -24;
                if (new_ref_db > 12)  new_ref_db = 12;
                body->mic_gain_db_plus24 = (uint8_t)(new_ref_db + 24);
                printf("[Config] Migrated mic gain reference: old %d dB -> new %+d dB\n", old_db, new_ref_db);
            }
            if (previous_body_config_version <= 5) {
                // DS5Dongle by Ohad 1.0.4: new AudioKeep setting. Existing
                // users get the Ohad default without changing other settings.
                body->keep_awake_on_audio = 1;
                printf("[Config] Migrated AudioKeep default ON\n");
            }
            if (previous_body_config_version <= 6) {
                // DS5Dongle by Ohad 1.0.5: new Language setting. Keep English
                // as the safe default for upgrades.
                body->ui_language = 0;
                printf("[Config] Migrated UI language default English\n");
            }
            if (previous_body_config_version <= 7) {
                // DS5Dongle by Ohad 1.0.6: new USB Mode setting. Preserve
                // normal Gamepad+Audio behavior for existing users.
                body->usb_mode = 0;
                printf("[Config] Migrated USB mode default Normal\n");
            }

            body->config_version = CONFIG_VERSION;
            printf("[Config] Preserved user settings while migrating schema %u -> %u\n",
                   previous_body_config_version, CONFIG_VERSION);
        } else {
            body->config_version = CONFIG_VERSION;
            // DS5Dongle by Ohad 1.0.5 Stable first-boot/factory defaults.
            body->polling_rate_mode = 2;
            body->lightbar_mode = 9;              // BATT
            body->auto_haptics_enable = 0;        // Haptics/AutoHap Off
            body->power_combo_enable = 1;         // PS+Options power combo On
            body->bt_connect_guard_100ms = 0;     // BT Guard removed/off
            body->out_burst_guard_100ms = 0;      // OUT Guard removed/off
            body->mic_gain_db_plus24 = 24;        // Mic Gain 0 dB reference (old -20 dB)
            body->remap_enable = 0;               // Remap Off
            body->screen_brightness = 5;          // OLED Bright 50%
            body->inactive_time = 5;              // fixed65u idle menu default: 5 min
            body->keep_awake_on_audio = 1;        // AudioKeep On
            body->ui_language = 0;                // English
            body->usb_mode = 0;                   // Normal HID+Audio USB mode
            printf("[Config] First boot / erased config, applying DS5Dongle by Ohad defaults\n");
        }
    }
}

void config_load() {
    Config primary{};
    memcpy(&primary, flash_config(), sizeof(Config));

    if (config_record_looks_valid(primary)) {
        config = primary;
        config_valid();
        return;
    }

    // One-time migration from old builds that stored Config in BTstack's last
    // flash bank. Do not erase/program the legacy sector; BTstack owns it.
    Config legacy{};
    memcpy(&legacy, legacy_flash_config(), sizeof(Config));
    if (config_record_looks_valid(legacy)) {
        printf("[Config] Migrating Settings from legacy BTstack-overlap sector to safe app sector\n");
        config = legacy;
        config_valid();
        config_write_flash_now();
        return;
    }

    // New sector is empty/invalid and legacy sector has no usable config.
    // Treat as first boot/factory defaults.
    config = primary;
    config_valid();
}

// Reset RAM-resident config body to firmware defaults. Caller must
// config_save() to persist. Filling with 0xFF mirrors the byte pattern
// of a freshly-erased flash sector, so every field fails validity and
// gets re-initialized to its documented default by config_valid().
void config_default() {
    memset(&config.body, 0xFF, sizeof(config.body));
    config_valid();
}

static bool config_write_flash_now() {
    config.crc32 = calc_config_crc(config);
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof(page));
    memcpy(page, &config, sizeof(Config));

    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(interrupts);

    Config verify{};
    memcpy(&verify, flash_config(), sizeof(verify));
    const auto verify_crc32 = calc_config_crc(verify);
    if (verify_crc32 == config.crc32) {
        printf("[Config] Config write flash verify success\n");
        return true;
    }
    printf("[Config] Config write flash verify failed\n");
    return false;
}

bool config_save() {
    // Normal menu/WebHID saves are audio-safe: update RAM now, write flash only
    // once the host is no longer streaming DS5 speaker or mic audio. During the
    // local poweroff sequence audio traffic is already frozen, so force the
    // write immediately to avoid losing pending settings before disconnect.
    if (!controller_poweroff_is_pending() && config_flash_audio_busy()) {
        g_config_save_deferred = true;
        g_config_audio_quiet_since_us = time_us_32();
        printf("[Config] Flash save deferred until USB audio is idle\n");
        return true;
    }

    const bool ok = config_write_flash_now();
    if (ok) g_config_save_deferred = false;
    return ok;
}

bool config_save_pending() {
    return g_config_save_deferred;
}

// DS5Dongle by Ohad 1.0.4:
// Force a previously deferred config save to flash right now. This is used by
// the local controller power-off path: if the user pressed Save while USB audio
// was active, config_save() correctly returned "Save pending" and settings_dirty
// was cleared, so the old power-off saver had nothing "dirty" left to commit.
// Without this flush, PS+Options/idle disconnect could tear down the controller
// before the deferred flash write ever happened.
bool config_flush_deferred_save_now() {
    if (!g_config_save_deferred) return true;
    const bool ok = config_write_flash_now();
    if (ok) g_config_save_deferred = false;
    return ok;
}

bool config_save_force_now() {
    const bool ok = config_write_flash_now();
    if (ok) g_config_save_deferred = false;
    return ok;
}

bool config_usb_midi_mode() {
    return config.body.usb_mode == 1;
}

void config_service_deferred_save() {
    if (!g_config_save_deferred) return;
    if (controller_poweroff_is_pending()) return;
    // Wait a small quiet window so games that briefly toggle the interface do
    // not get a flash write in the middle of their next audio burst.
    if (!config_flash_audio_quiet_for(300000u)) return;

    if (config_write_flash_now()) {
        g_config_save_deferred = false;
        printf("[Config] Deferred flash save completed\n");
    }
}

const Config_body& get_config() {
    return config.body;
}

void set_config(const uint8_t *new_config, const uint16_t len) {
    const auto copy_len = len < sizeof(Config_body) ? len : sizeof(Config_body);
    memcpy(&config.body, new_config, copy_len);
    config_valid();
    if (config.body.disable_pico_led) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    }else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    }
}

void set_config(const Config_body &new_config) {
    config.body = new_config;
    config_valid();
    if (config.body.disable_pico_led) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    } else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    }
}
