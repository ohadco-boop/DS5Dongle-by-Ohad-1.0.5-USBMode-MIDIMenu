#include "oled.h"
#include "oled_font.h"
#include "bt.h"
#include "ma2_telnet.h"
#include "telnet_settings.h"
#include "usb_net_lwip.h"
#include "state_mgr.h"
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

uint8_t pad_prev = 8;
bool tri_prev = false;
bool mute_prev = false;
bool settings_mode = false;
uint32_t pad_first_us = 0;
uint32_t pad_last_repeat_us = 0;
uint8_t pad_repeat_dir = 8;
constexpr uint32_t kPadRepeatDelayUs = 450000;
constexpr uint32_t kPadRepeatUs = 130000;

int field = 0;
constexpr int FIELD_PICO_IP0 = 0;
constexpr int FIELD_MA2_IP0 = 4;
constexpr int FIELD_USER0 = 8;
constexpr int FIELD_PASS0 = 23;
constexpr int FIELD_DEADZONE = 38;
constexpr int FIELD_SPEED1 = 39;
constexpr int FIELD_SPEED2 = 40;
constexpr int FIELD_STEP1 = 41;
constexpr int FIELD_STEP2 = 42;
constexpr int FIELD_STEP3 = 43;
constexpr int FIELD_RATE1 = 44;
constexpr int FIELD_RATE2 = 45;
constexpr int FIELD_RATE3 = 46;
constexpr int FIELD_TOTAL = 47;

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

void format_step_text(uint16_t x10, char* out, size_t out_len) {
    if (x10 % 10 == 0) std::snprintf(out, out_len, "%u", (unsigned)(x10 / 10));
    else std::snprintf(out, out_len, "%u.%u", (unsigned)(x10 / 10), (unsigned)(x10 % 10));
}

void normalize_string(char s[16]) {
    s[15] = 0;
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

void change_field_value(int delta) {
    if (delta == 0) return;
    Ma2TelnetSettings s = telnet_settings_get();
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) {
        uint8_t& v = s.pico_ip[field - FIELD_PICO_IP0];
        v = (uint8_t)(v + delta);
    } else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) {
        uint8_t& v = s.ma2_ip[field - FIELD_MA2_IP0];
        v = (uint8_t)(v + delta);
        s.gateway[0] = s.ma2_ip[0]; s.gateway[1] = s.ma2_ip[1]; s.gateway[2] = s.ma2_ip[2]; s.gateway[3] = s.ma2_ip[3];
    } else if (field >= FIELD_USER0 && field < FIELD_USER0 + 15) {
        const int pos = field - FIELD_USER0;
        int idx = char_index(s.username[pos]);
        const int n = (int)std::strlen(kChars);
        idx = (idx + delta) % n;
        if (idx < 0) idx += n;
        s.username[pos] = kChars[idx];
        normalize_string(s.username);
    } else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 15) {
        const int pos = field - FIELD_PASS0;
        int idx = char_index(s.password[pos]);
        const int n = (int)std::strlen(kChars);
        idx = (idx + delta) % n;
        if (idx < 0) idx += n;
        s.password[pos] = kChars[idx];
        normalize_string(s.password);
    } else if (field == FIELD_DEADZONE) {
        int v = (int)s.deadzone_percent + delta;
        if (v < 0) v = 30;
        if (v > 30) v = 0;
        s.deadzone_percent = (uint8_t)v;
    } else if (field == FIELD_SPEED1) {
        int v = (int)s.speed1_percent + delta;
        if (v < 10) v = 80;
        if (v > 80) v = 10;
        s.speed1_percent = (uint8_t)v;
        if (s.speed2_percent <= s.speed1_percent) s.speed2_percent = (uint8_t)(s.speed1_percent + 1);
        if (s.speed2_percent > 95) s.speed2_percent = 95;
    } else if (field == FIELD_SPEED2) {
        int v = (int)s.speed2_percent + delta;
        const int min_v = (int)s.speed1_percent + 1;
        if (v < min_v) v = 95;
        if (v > 95) v = min_v;
        s.speed2_percent = (uint8_t)v;
    } else if (field >= FIELD_STEP1 && field <= FIELD_STEP3) {
        const int idx = field - FIELD_STEP1;
        int v = (int)s.step_x10[idx] + delta;
        if (v < 1) v = 200;
        if (v > 200) v = 1;
        s.step_x10[idx] = (uint16_t)v;
    } else if (field >= FIELD_RATE1 && field <= FIELD_RATE3) {
        const int idx = field - FIELD_RATE1;
        int v = (int)s.rate_ms[idx] + delta * 5;
        if (v < 20) v = 500;
        if (v > 500) v = 20;
        s.rate_ms[idx] = (uint16_t)v;
    }
    telnet_settings_set(s);
    last_render_us = 0;
}

void increment_field() { change_field_value(+1); }
void decrement_field() { change_field_value(-1); }

void move_field(int delta) {
    field = (field + delta) % FIELD_TOTAL;
    if (field < 0) field += FIELD_TOTAL;
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
        if (!chord_done) move_field(+1);
    }
    if (!key1_prev && k1 && (uint32_t)(now - key1_down_us) > kDebounceUs) {
        if (!key1_long_done && !chord_done) increment_field();
    }

    if (k0 && k1) chord_done = false;
    key0_prev = k0;
    key1_prev = k1;
}

void field_label(char* out, size_t out_len) {
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) std::snprintf(out, out_len, "Pico IP octet %d", field - FIELD_PICO_IP0 + 1);
    else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) std::snprintf(out, out_len, "MA2 IP octet %d", field - FIELD_MA2_IP0 + 1);
    else if (field >= FIELD_USER0 && field < FIELD_USER0 + 15) std::snprintf(out, out_len, "User char %02d", field - FIELD_USER0);
    else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 15) std::snprintf(out, out_len, "Pass char %02d", field - FIELD_PASS0);
    else if (field == FIELD_DEADZONE) std::snprintf(out, out_len, "Deadzone");
    else if (field == FIELD_SPEED1) std::snprintf(out, out_len, "Zone 1/2 edge");
    else if (field == FIELD_SPEED2) std::snprintf(out, out_len, "Zone 2/3 edge");
    else if (field == FIELD_STEP1) std::snprintf(out, out_len, "Zone 1 step");
    else if (field == FIELD_STEP2) std::snprintf(out, out_len, "Zone 2 step");
    else if (field == FIELD_STEP3) std::snprintf(out, out_len, "Zone 3 step");
    else if (field == FIELD_RATE1) std::snprintf(out, out_len, "Zone 1 rate");
    else if (field == FIELD_RATE2) std::snprintf(out, out_len, "Zone 2 rate");
    else std::snprintf(out, out_len, "Zone 3 rate");
}

void current_value_text(char* out, size_t out_len) {
    const auto& s = telnet_settings_get();
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) {
        std::snprintf(out, out_len, "Pico:%03u.%03u.%03u.%03u", s.pico_ip[0], s.pico_ip[1], s.pico_ip[2], s.pico_ip[3]);
    } else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) {
        std::snprintf(out, out_len, "MA2 :%03u.%03u.%03u.%03u", s.ma2_ip[0], s.ma2_ip[1], s.ma2_ip[2], s.ma2_ip[3]);
    } else if (field >= FIELD_USER0 && field < FIELD_USER0 + 15) {
        std::snprintf(out, out_len, "User:%-15s", s.username[0] ? s.username : "<blank>");
    } else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 15) {
        std::snprintf(out, out_len, "Pass:%-15s", s.password[0] ? s.password : "<blank>");
    } else if (field == FIELD_DEADZONE) {
        std::snprintf(out, out_len, "Deadzone:%u%%", s.deadzone_percent);
    } else if (field == FIELD_SPEED1) {
        std::snprintf(out, out_len, "Z1/Z2 at:%u%%", s.speed1_percent);
    } else if (field == FIELD_SPEED2) {
        std::snprintf(out, out_len, "Z2/Z3 at:%u%%", s.speed2_percent);
    } else if (field >= FIELD_STEP1 && field <= FIELD_STEP3) {
        char st[12];
        const int idx = field - FIELD_STEP1;
        format_step_text(s.step_x10[idx], st, sizeof(st));
        std::snprintf(out, out_len, "Step %d:%s", idx + 1, st);
    } else {
        const int idx = field - FIELD_RATE1;
        std::snprintf(out, out_len, "Rate %d:%ums", idx + 1, s.rate_ms[idx]);
    }
}

void draw_edit_cursor() {
    int x = -1;
    if (field >= FIELD_PICO_IP0 && field < FIELD_PICO_IP0 + 4) {
        const int oct = field - FIELD_PICO_IP0;
        x = 5 * 6 + oct * 4 * 6;
    } else if (field >= FIELD_MA2_IP0 && field < FIELD_MA2_IP0 + 4) {
        const int oct = field - FIELD_MA2_IP0;
        x = 5 * 6 + oct * 4 * 6;
    } else if (field >= FIELD_USER0 && field < FIELD_USER0 + 15) {
        x = 5 * 6 + (field - FIELD_USER0) * 6;
    } else if (field >= FIELD_PASS0 && field < FIELD_PASS0 + 15) {
        x = 5 * 6 + (field - FIELD_PASS0) * 6;
    }
    if (x >= 0 && x < kW - 5) for (int i = 0; i < 5; ++i) pix(x + i, 55);
}

void render() {
    fb_clear();
    char line[40];

    if ((int32_t)(time_us_32() - popup_until_us) < 0) {
        text(0, 0, "DS5 -> MA2 Telnet");
        rect(0, 18, 127, 28);
        text(8, 28, popup_msg);
        flush();
        return;
    }

    text(0, 0, "DS5 MA2 TELNET");
    std::snprintf(line, sizeof(line), "BT:%s T:%s", bt_is_connected() ? "OK" : "--", ma2_telnet_logged_in() ? "READY" : "----");
    text(0, 9, line);
    text(0, 18, ma2_telnet_status());

    if (!settings_mode) {
        text(0, 29, "MODE: MA2 NAV");
        text(0, 38, "Mute: settings ON");
        text(0, 49, "Dpad->MA2 arrows");
        text(0, 57, "RStick Pan/Tilt");
        flush();
        return;
    }

    text(0, 29, "MODE: SETTINGS");

    char lbl[28];
    field_label(lbl, sizeof(lbl));
    text(0, 38, lbl);

    char val[40];
    current_value_text(val, sizeof(val));
    text(0, 48, val);
    draw_edit_cursor();

    text(0, 57, "Dpad edit  Tri save");
    flush();
}

void controller_nav_event(uint8_t dpad, bool triangle) {
    const uint32_t now = time_us_32();

    if (triangle && !tri_prev) save_settings();
    tri_prev = triangle;

    auto step = [](uint8_t dir) {
        switch (dir) {
            case 0: move_field(-1); break;      // Up
            case 4: move_field(+1); break;      // Down
            case 6: decrement_field(); break;   // Left
            case 2: increment_field(); break;   // Right
            default: break;
        }
    };

    const bool is_cardinal = (dpad == 0 || dpad == 2 || dpad == 4 || dpad == 6);
    if (is_cardinal && dpad != pad_prev) {
        step(dpad);
        pad_first_us = now;
        pad_last_repeat_us = now;
        pad_repeat_dir = dpad;
    } else if (is_cardinal && dpad == pad_repeat_dir) {
        if ((uint32_t)(now - pad_first_us) >= kPadRepeatDelayUs &&
            (uint32_t)(now - pad_last_repeat_us) >= kPadRepeatUs) {
            step(dpad);
            pad_last_repeat_us = now;
        }
    } else if (!is_cardinal) {
        pad_repeat_dir = 8;
    }
    pad_prev = dpad;
}
} // namespace

void oled_handle_controller_report(const uint8_t report[63]) {
    if (!report) return;

    const bool mute = (report[9] & 0x04) != 0;
    if (mute && !mute_prev) {
        settings_mode = !settings_mode;
        state_set_mute_light(settings_mode);
        state_push_current_to_controller();
        oled_show_message(settings_mode ? "SETTINGS MODE" : "MA2 NAV MODE", 800);
        pad_prev = 8;
        pad_repeat_dir = 8;
        tri_prev = false;
    }
    mute_prev = mute;

    if (!settings_mode) return;

    const uint8_t dpad = report[7] & 0x0F;
    const bool triangle = (report[7] & 0x80) != 0;
    controller_nav_event(dpad, triangle);
}

bool oled_settings_mode_active() { return settings_mode; }

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
