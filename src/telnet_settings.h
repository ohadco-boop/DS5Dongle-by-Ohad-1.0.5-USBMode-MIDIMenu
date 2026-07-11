#pragma once
#include <cstdint>

// Controller buttons that can be mapped to MA2 hardkeys/Telnet commands.
// Mute is intentionally not here; it is reserved for Settings mode toggle.
enum Ma2ButtonMapSlot : uint8_t {
    MA2_BTN_DPAD_UP = 0,
    MA2_BTN_DPAD_RIGHT,
    MA2_BTN_DPAD_DOWN,
    MA2_BTN_DPAD_LEFT,
    MA2_BTN_SQUARE,
    MA2_BTN_CROSS,
    MA2_BTN_CIRCLE,
    MA2_BTN_TRIANGLE,
    MA2_BTN_L1,
    MA2_BTN_R1,
    MA2_BTN_L2,
    MA2_BTN_R2,
    MA2_BTN_CREATE,
    MA2_BTN_OPTIONS,
    MA2_BTN_L3,
    MA2_BTN_R3,
    MA2_BTN_PS,
    MA2_BTN_TOUCHPAD,
    MA2_BTN_COUNT
};

// Values stored in settings.button_map[]. 0 = disabled.
enum Ma2HardkeyMapValue : uint8_t {
    MA2_HK_DISABLED = 0,
    MA2_HK_UP,
    MA2_HK_DOWN,
    MA2_HK_LEFT,
    MA2_HK_RIGHT,
    MA2_HK_NEXT,
    MA2_HK_PREVIOUS,
    MA2_HK_GO_PLUS,
    MA2_HK_GO_MINUS,
    MA2_HK_PAUSE,
    MA2_HK_TOP,
    MA2_HK_ON,
    MA2_HK_OFF,
    MA2_HK_CLEAR,
    MA2_HK_STORE,
    MA2_HK_UPDATE,
    MA2_HK_DELETE,
    MA2_HK_EDIT,
    MA2_HK_ASSIGN,
    MA2_HK_MOVE,
    MA2_HK_COPY,
    MA2_HK_FIXTURE,
    MA2_HK_CHANNEL,
    MA2_HK_GROUP,
    MA2_HK_PRESET,
    MA2_HK_SEQUENCE,
    MA2_HK_CUE,
    MA2_HK_EXECUTOR,
    MA2_HK_PAGE,
    MA2_HK_SELECT,
    MA2_HK_TEMP,
    MA2_HK_HIGHLIGHT,
    MA2_HK_SOLO,
    MA2_HK_BLACKOUT,
    MA2_HK_FREEZE,
    MA2_HK_ESC,
    MA2_HK_PLEASE,
    MA2_HK_MA,
    MA2_HK_OOPS,
    MA2_HK_COUNT
};


struct Ma2TelnetSettings {
    uint32_t magic;
    uint8_t pico_ip[4];     // Pico USB network IP, default 192.168.7.2
    uint8_t netmask[4];     // default 255.255.255.0
    uint8_t gateway[4];     // usually PC USB side, default 192.168.7.1
    uint8_t ma2_ip[4];      // MA2/onPC Telnet target, default 192.168.7.1
    uint16_t ma2_port;      // MA2 Telnet port, default 30000
    uint8_t deadzone_percent;
    uint8_t speed1_percent; // speed 1/2 boundary
    uint8_t speed2_percent; // speed 2/3 boundary
    uint16_t step_x10[3];   // Pan/Tilt step per zone, fixed point: 1 = 0.1, 200 = 20.0
    uint16_t rate_ms[3];    // repeat rate per zone in milliseconds
    char username[16];      // default administrator
    char password[16];      // default admin
    uint8_t button_map[MA2_BTN_COUNT]; // Ma2HardkeyMapValue per Ma2ButtonMapSlot
};

Ma2TelnetSettings telnet_settings_default();
void telnet_settings_load();
void telnet_settings_save();
const Ma2TelnetSettings& telnet_settings_get();
void telnet_settings_set(const Ma2TelnetSettings& s);
