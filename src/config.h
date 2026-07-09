//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CONFIG_H
#define DS5_BRIDGE_CONFIG_H

#include <cstdint>

struct __attribute__((packed)) Config_body {
    uint8_t config_version; // Config Version
    float haptics_gain; // [1.0,2.0]
    float speaker_volume; // [-100,0]
    uint8_t inactive_time; // fixed65u idle choices: 1/2/3/5/10/20/30 min; Off uses disable_inactive_disconnect
    uint8_t disable_inactive_disconnect; // bool: 0 idle auto-disconnect on, 1 Idle off
    uint8_t disable_pico_led; // bool
    uint8_t polling_rate_mode; // 0: 250Hz, 1: 500Hz, 2: real-time
    uint8_t audio_buffer_length; // [16,128]
    uint8_t controller_mode; // 0: DS5, 1: DSE, 2: Auto
    uint8_t current_slot;    // [0..3] active multi-pairing slot (OLED Edition Phase G)
    // Audio Auto Haptics — derive haptic feedback from speaker audio for games that
    // send no per-frame native haptic data. DSP borrowed from loteran/DS5Dongle 5d6bc2f.
    uint8_t auto_haptics_enable;  // 0=Off (default from fixed65n), 1=Fallback, 2=Mix, 3=Replace
    uint8_t auto_haptics_gain;    // [0,200] percent, default 100
    uint8_t auto_haptics_lowpass; // 0=80Hz, 1=160Hz (default), 2=250Hz, 3=400Hz
    // Lightbar fixed65: only 8=HOST (host/game owns LED) and 9=BATT
    // (Pico battery blue->red 25%). Legacy 0..7 modes are no longer exposed;
    // erased/invalid flash defaults to BATT.
    uint8_t lightbar_mode;
    uint8_t lb_fav_r[4];
    uint8_t lb_fav_g[4];
    uint8_t lb_fav_b[4];
    // OLED idle power-ladder thresholds, in minutes. 0 = that tier disabled.
    // Defaults preserve the original hardcoded ladder (2 min dim, 15 min off).
    // Range [0,250] (0xFF erased flash → default via config_valid clamp). The
    // idle timer is 64-bit µs so the full range is representable. Issue #5.
    uint8_t screen_dim_timeout;
    uint8_t screen_off_timeout;
    // DualSense mic over Bluetooth (Phase I). 0 = off, 1 = on (default). When on,
    // the dongle asserts the DS5 mic-enable bit so the controller streams its mic
    // over BT and the dongle decodes it to the USB capture endpoint. Costs extra
    // DS5 battery (keeps its audio subsystem awake), hence the toggle.
    uint8_t bt_mic_enable;
    // OLED brightness, as an index into kBrightLevels[] (src/oled.cpp).
    // fixed65b: 0=100%, 9=10%. Persisted so the Settings/KEY1 choice
    // survives a power cycle. Erased flash (0xFF) → 5 (50%).
    uint8_t screen_brightness;
    // When 0, controller input no longer keeps the OLED awake — only the OLED's
    // own KEY0/KEY1 do — so the dim/off timers actually count down during
    // gameplay and the panel can sleep while the controller is in use. Default 1
    // preserves the original "any controller activity wakes the screen"
    // behavior. Issues #8 (dim timeout never fired during play) and #9.
    uint8_t controller_wakes_display;
    // OLED remap menu: 0=passthrough/disabled, 1=apply the persisted
    // per-button remap table from remap.cpp. Kept in the same config byte
    // used by older Ohad custom builds, but the hardcoded 3-button mapping
    // is removed. The new Remap OLED screen owns this setting.
    uint8_t remap_enable;
    // Ohad fixed38: PS+Options controller power-off combo toggle.
    // 0=disabled, 1=enabled from OLED Settings (default from fixed65n).
    uint8_t power_combo_enable;
    // OLED orientation: 0=normal, 1=flip 180 degrees. Appended at the end
    // so older saved configs keep every existing field at the same offset.
    uint8_t screen_rotation;
    // fixed65ak: legacy guard bytes kept for flash/config compatibility,
    // but both are forced to 0/off and hidden from the OLED menu.
    uint8_t bt_connect_guard_100ms;
    uint8_t out_burst_guard_100ms;
    // DS5Dongle by Ohad 1.0.0 Stable: user relative software gain for decoded
    // BT mic before USB capture. Stored as display dB + 24:
    // 0=-24 dB, 24=0 dB default, 36=+12 dB.
    // Runtime reference is shifted down 20 dB, so display 0 dB equals the
    // old fixed65am Mic Gain -20 dB level.
    uint8_t mic_gain_db_plus24;
    // DS5Dongle by Ohad 1.0.4: when enabled, active USB audio prevents the
    // physical-input idle timer from powering off the controller. This lets the
    // controller stay connected while listening to music, but still powers off
    // after idle once the audio stream stops.
    uint8_t keep_awake_on_audio; // bool: 0=normal idle poweroff, 1=audio keeps awake
    // DS5Dongle by Ohad 1.0.5: OLED UI language. 0=English, 1=Hebrew.
    // English remains the safe default so existing users keep the old UI after upgrade.
    uint8_t ui_language;
    // DS5Dongle by Ohad 1.0.6: USB mode selected from OLED Settings.
    // 0=normal DualSense-compatible HID+Audio, 1=USB MIDI only for MA2.
    // Changing this requires a reboot so TinyUSB re-enumerates with the other descriptor.
    uint8_t usb_mode;
};

struct __attribute__((packed)) Config {
    uint32_t magic;
    uint16_t version;
    uint32_t crc32; // Config_body crc32, only calc and verify when save
    uint16_t size;  // Config_body size
    Config_body body;
};

void config_default();
void config_load();
bool config_save();
bool config_save_pending();
bool config_flush_deferred_save_now();
bool config_save_force_now();
bool config_usb_midi_mode();
void config_service_deferred_save();
const Config_body& get_config();
void set_config(const uint8_t *new_config, const uint16_t len);
void config_valid();
void set_config(const Config_body &new_config);
extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
