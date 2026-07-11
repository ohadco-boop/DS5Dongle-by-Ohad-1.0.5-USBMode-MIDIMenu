#include "oled.h"
#include "oled_font.h"
#include "bt.h"
#include "ma2_telnet.h"
#include "telnet_settings.h"
#include "usb_net_lwip.h"
#include <cstdio>
#include <cstring>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdlib.h"

extern volatile uint32_t g_ds_reports;
bool g_lightbar_override = false;

namespace {
constexpr uint kPinDC = 8;
constexpr uint kPinCS = 9;
constexpr uint kPinCLK = 10;
constexpr uint kPinMOSI = 11;
constexpr uint kPinRST = 12;
constexpr uint kPinKey0 = 15;
constexpr uint kPinKey1 = 17;

constexpr int kW = 128;
constexpr int kH = 64;
constexpr int kRowBytes = kW / 8;
constexpr int kFbBytes = kRowBytes * kH;
uint8_t fb[kFbBytes];
uint32_t last_render_us = 0;
uint32_t popup_until_us = 0;
char popup_msg[32] = "";

bool key0_prev = true, key1_prev = true;
uint32_t key0_down_us = 0, key1_down_us = 0;
bool key1_long_done = false;
bool chord_done = false;
constexpr uint32_t kDebounceUs = 25000;
constexpr uint32_t kLongUs = 900000;

int field = 0;
constexpr int FIELD_PICO_IP0 = 0;
constexpr int FIELD_MA2_IP0 = 4;
constexpr int FIELD_USER0 = 8;
constexpr int FIELD_PASS0 = 24;
constexpr int FIELD_DEADZONE = 40;
constexpr int FIELD_SPEED1 = 41;
constexpr int FIELD_SPEED2 = 42;
constexpr int FIELD_TOTAL = 43;

const char* kChars = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-@!#";

void cmd(uint8_t c) {
    gpio_put(kPinDC, 0);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &c, 1);
    gpio_put(kPinCS, 1);
}

void data_byte(uint8_t d) {
    gpio_put(kPinDC, 1);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &d, 1);
    gpio_put(kPinCS, 1);
}

uint8_t reverse_byte(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

void hw_reset() {
    gpio_put(kPinRST, 1); sleep_ms(40);
    gpio_put(kPinRST, 0); sleep_ms(40);
    gpio_put(kPinRST, 1); sleep_ms(80);
}

void sh1107_init() {
    cmd(0xAE);
    cmd(0x00); cmd(0x10);
    cmd(0xB0);
    cmd(0xDC); cmd(0x00);
    cmd(0x81); cmd(0xAF);
    cmd(0x21);
    cmd(0xA0);
    cmd(0xC0);
    cmd(0xA4);
    cmd(0xA6);
    cmd(0xA8); cmd(0x3F);
    cmd(0xD3); cmd(0x60);
    cmd(0xD5); cmd(0x41);
    cmd(0xD9); cmd(0x22);
    cmd(0xDB); cmd(0x35);
    cmd(0xAD); cmd(0x8A);
    sleep_ms(50);
    cmd(0xAF);
}

void fb_clear() { std::memset(fb, 0, sizeof(fb)); }

void pix(int x, int y, bool on = true) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    // Match the proven framebuffer bit order from the original OLED firmware.
    // The SH1107 flush path reverses each byte before sending it, so pixels
    // must be stored MSB-first in the framebuffer. Using LSB-first here makes
    // the whole screen look scrambled/mirrored.
    uint8_t& b = fb[y * kRowBytes + (x >> 3)];
    uint8_t m = (uint8_t)(1u << (7 - (x & 7)));
    if (on) b |= m; else b &= (uint8_t)~m;
}

void rect(int x, int y, int w, int h) {
    for (int i = x; i < x + w; ++i) { pix(i, y); pix(i, y + h - 1); }
    for (int j = y; j < y + h; ++j) { pix(x, j); pix(x + w - 1, j); }
}

void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = kFont5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) if (bits & (1u << row)) pix(x + col, y + row);
    }
}

void text(int x, int y, const char* s) {
    for (; *s && x < kW - 5; ++s, x += 6) draw_char(x, y, *s);
}

void flush() {
    cmd(0xB0);
    for (int j = 0; j < kH; j++) {
        const uint8_t col = kH - 1 - j;
        cmd(0x00 + (col & 0x0F));
        cmd(0x10 + (col >> 4));
        for (int i = 0; i < kRowBytes; i++) data_byte(reverse_byte(fb[j * kRowBytes + i]));
    }
}

void ip_to_text(const uint8_t ip[4], char* out, size_t out_len) {
    std::snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

int char_index(char c) {
    const char* p = std::strchr(kChars, c ? c : ' ');
    return p ? (int)(p - kChars) : 0;
}

void normalize_string(char s[16]) {
    s[15] = 0;
    // Treat trailing spaces as empty, so the 2-button editor can shorten strings.
    for (int i = 14; i >= 0; --i) {
        if (s[i] == ' ') s[i] = 0;
        else if (s[i] != 0) break;
    }
    for (int i = 0; i < 15; ++i) {
        if (s[i] == 0) {
            for (int j = i + 1; j < 16; ++j) s[j] = 0;
            return;
        }
    }
}

void increment_field() {
    Ma2TelnetSettings s = telnet_settings_get();
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) {
        uint8_t& v = s.pico_ip[field - FIELD_PICO_IP0];
        v = (uint8_t)(v + 1);
    } else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) {
        uint8_t& v = s.ma2_ip[field - FIELD_MA2_IP0];
        v = (uint8_t)(v + 1);
        s.gateway[0] = s.ma2_ip[0]; s.gateway[1] = s.ma2_ip[1]; s.gateway[2] = s.ma2_ip[2]; s.gateway[3] = s.ma2_ip[3];
    } else if (field >= FIELD_USER0 && field < FIELD_USER0 + 16) {
        int pos = field - FIELD_USER0;
        int idx = char_index(s.username[pos]);
        idx = (idx + 1) % (int)std::strlen(kChars);
        s.username[pos] = kChars[idx];
        normalize_string(s.username);
    } else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 16) {
        int pos = field - FIELD_PASS0;
        int idx = char_index(s.password[pos]);
        idx = (idx + 1) % (int)std::strlen(kChars);
        s.password[pos] = kChars[idx];
        normalize_string(s.password);
    } else if (field == FIELD_DEADZONE) {
        s.deadzone_percent = (uint8_t)((s.deadzone_percent + 1) % 16);
    } else if (field == FIELD_SPEED1) {
        s.speed1_percent = (uint8_t)(10 + ((s.speed1_percent - 9) % 70));
        if (s.speed2_percent <= s.speed1_percent) s.speed2_percent = s.speed1_percent + 1;
    } else if (field == FIELD_SPEED2) {
        s.speed2_percent = (uint8_t)(s.speed1_percent + 1 + ((s.speed2_percent - s.speed1_percent) % (95 - s.speed1_percent)));
    }
    telnet_settings_set(s);
    last_render_us = 0;
}

void save_settings() {
    telnet_settings_save();
    usb_net_lwip_reconfigure();
    ma2_telnet_reconfigure();
    oled_show_message("Saved + reconnect", 1200);
}

void reset_defaults() {
    telnet_settings_set(telnet_settings_default());
    telnet_settings_save();
    usb_net_lwip_reconfigure();
    ma2_telnet_reconfigure();
    oled_show_message("Defaults restored", 1200);
}

void handle_buttons() {
    const uint32_t now = time_us_32();
    bool k0 = gpio_get(kPinKey0);
    bool k1 = gpio_get(kPinKey1);

    if (!k0 && !key0_prev) {
        if (!k1 && !key1_prev && !chord_done && (uint32_t)(now - key0_down_us) > kLongUs && (uint32_t)(now - key1_down_us) > kLongUs) {
            chord_done = true;
            reset_defaults();
        }
    }

    if (key0_prev && !k0) key0_down_us = now;
    if (key1_prev && !k1) { key1_down_us = now; key1_long_done = false; }

    if (!k1 && !key1_long_done && (uint32_t)(now - key1_down_us) > kLongUs) {
        key1_long_done = true;
        save_settings();
    }

    if (!key0_prev && k0 && (uint32_t)(now - key0_down_us) > kDebounceUs) {
        if (!chord_done) {
            field = (field + 1) % FIELD_TOTAL;
            last_render_us = 0;
        }
    }
    if (!key1_prev && k1 && (uint32_t)(now - key1_down_us) > kDebounceUs) {
        if (!key1_long_done && !chord_done) increment_field();
    }

    if (k0 && k1) chord_done = false;
    key0_prev = k0;
    key1_prev = k1;
}

void field_label(char* out, size_t out_len) {
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) std::snprintf(out, out_len, "Edit Pico IP.%d", field - FIELD_PICO_IP0 + 1);
    else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) std::snprintf(out, out_len, "Edit MA2 IP.%d", field - FIELD_MA2_IP0 + 1);
    else if (field >= FIELD_USER0 && field < FIELD_USER0 + 16) std::snprintf(out, out_len, "Edit User[%02d]", field - FIELD_USER0);
    else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 16) std::snprintf(out, out_len, "Edit Pass[%02d]", field - FIELD_PASS0);
    else if (field == FIELD_DEADZONE) std::snprintf(out, out_len, "Edit Deadzone");
    else if (field == FIELD_SPEED1) std::snprintf(out, out_len, "Edit Speed 1/2");
    else std::snprintf(out, out_len, "Edit Speed 2/3");
}

void render() {
    fb_clear();
    const auto& s = telnet_settings_get();
    char line[40];
    char ip[24];

    if ((int32_t)(time_us_32() - popup_until_us) < 0) {
        text(0, 0, "DS5 -> MA2 Telnet");
        rect(0, 18, 127, 28);
        text(8, 28, popup_msg);
        flush();
        return;
    }

    text(0, 0, "DS5 MA2 TELNET USB");
    std::snprintf(line, sizeof(line), "BT:%s  %s", bt_is_connected() ? "OK" : "WAIT", ma2_telnet_logged_in() ? "READY" : "----");
    text(0, 10, line);
    text(0, 20, ma2_telnet_status());

    ip_to_text(s.pico_ip, ip, sizeof(ip));
    std::snprintf(line, sizeof(line), "Pico %s", ip);
    text(0, 31, line);
    ip_to_text(s.ma2_ip, ip, sizeof(ip));
    std::snprintf(line, sizeof(line), "MA2  %s", ip);
    text(0, 40, line);

    char lbl[24];
    field_label(lbl, sizeof(lbl));
    text(0, 51, lbl);
    text(0, 59, "K0 next K1 + hold=save");
    flush();
}
}

void oled_show_message(const char *msg, uint32_t duration_ms) {
    std::snprintf(popup_msg, sizeof(popup_msg), "%s", msg ? msg : "");
    popup_until_us = time_us_32() + duration_ms * 1000u;
    last_render_us = 0;
}

void oled_init() {
    spi_init(spi1, 10 * 1000 * 1000);
    gpio_set_function(kPinCLK, GPIO_FUNC_SPI);
    gpio_set_function(kPinMOSI, GPIO_FUNC_SPI);
    gpio_init(kPinCS); gpio_set_dir(kPinCS, GPIO_OUT); gpio_put(kPinCS, 1);
    gpio_init(kPinDC); gpio_set_dir(kPinDC, GPIO_OUT); gpio_put(kPinDC, 0);
    gpio_init(kPinRST); gpio_set_dir(kPinRST, GPIO_OUT); gpio_put(kPinRST, 1);
    gpio_init(kPinKey0); gpio_set_dir(kPinKey0, GPIO_IN); gpio_pull_up(kPinKey0);
    gpio_init(kPinKey1); gpio_set_dir(kPinKey1, GPIO_IN); gpio_pull_up(kPinKey1);
    hw_reset();
    sh1107_init();
    oled_show_message("MA2 Telnet USBNet", 1200);
}

void oled_loop() {
    handle_buttons();
    const uint32_t now = time_us_32();
    if ((uint32_t)(now - last_render_us) < 100000) return;
    last_render_us = now;
    render();
}

void oled_return_to_status_screen() { field = 0; last_render_us = 0; }
bool oled_save_pending_changes_for_poweroff() { save_settings(); return true; }
bool oled_handle_controller_screen_nav_shortcut() { return false; }
bool oled_lightbar_host_mode() { return false; }
