#pragma once
#include <cstdint>

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
    char password[16];      // default empty
};

Ma2TelnetSettings telnet_settings_default();
void telnet_settings_load();
void telnet_settings_save();
const Ma2TelnetSettings& telnet_settings_get();
void telnet_settings_set(const Ma2TelnetSettings& s);
