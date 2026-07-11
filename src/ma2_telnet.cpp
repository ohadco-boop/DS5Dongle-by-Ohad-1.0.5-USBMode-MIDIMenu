#include "ma2_telnet.h"
#include "telnet_settings.h"
#include "oled.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "pico/time.h"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace {
tcp_pcb* g_pcb = nullptr;
bool g_connected = false;
bool g_logged_in = false;
bool g_login_sent = false;
uint32_t g_connected_at_us = 0;
uint32_t g_next_connect_us = 0;
uint32_t g_next_login_us = 0;
char g_status[32] = "TELNET idle";

uint32_t g_last_dir_send_us[4] = {0, 0, 0, 0};
uint8_t g_last_dir_speed[4] = {0, 0, 0, 0};
uint8_t g_nav_prev_dpad = 8;
uint8_t g_nav_repeat_dir = 8;
uint32_t g_nav_first_us = 0;
uint32_t g_nav_last_us = 0;

constexpr uint32_t kRetryUs = 2000000;
constexpr uint32_t kLoginDelayUs = 250000;
constexpr uint32_t kLoginAssumeReadyUs = 900000;

constexpr uint8_t IAC  = 255;
constexpr uint8_t DONT = 254;
constexpr uint8_t DO   = 253;
constexpr uint8_t WONT = 252;
constexpr uint8_t WILL = 251;
constexpr uint8_t SB   = 250;
constexpr uint8_t SE   = 240;
constexpr uint8_t TEL_ECHO = 1;
constexpr uint8_t TEL_SGA  = 3;
constexpr uint8_t TEL_TTYPE = 24;
constexpr uint8_t TEL_TTYPE_IS = 0;
constexpr uint8_t TEL_TTYPE_SEND = 1;

void set_status(const char* s) { std::snprintf(g_status, sizeof(g_status), "%s", s); }

void close_conn() {
    if (g_pcb) {
        tcp_arg(g_pcb, nullptr);
        tcp_recv(g_pcb, nullptr);
        tcp_sent(g_pcb, nullptr);
        tcp_err(g_pcb, nullptr);
        tcp_close(g_pcb);
        g_pcb = nullptr;
    }
    g_connected = false;
    g_logged_in = false;
    g_login_sent = false;
    set_status("TELNET closed");
    g_next_connect_us = time_us_32() + kRetryUs;
}

void send_raw_bytes(const uint8_t* data, uint16_t len) {
    if (!g_pcb || !data || len == 0) return;
    if (tcp_sndbuf(g_pcb) < len) return;
    tcp_write(g_pcb, data, len, TCP_WRITE_FLAG_COPY);
    tcp_output(g_pcb);
}

void send_raw(const char* text) {
    if (!g_pcb || !text) return;
    const size_t len = std::strlen(text);
    if (len == 0) return;
    if (tcp_sndbuf(g_pcb) < len) return;
    tcp_write(g_pcb, text, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    tcp_output(g_pcb);
}

void send_line(const char* line) {
    send_raw(line);
    send_raw("\r\n");
}

void append_quoted(char* out, size_t out_sz, size_t& pos, const char* v) {
    if (pos + 1 < out_sz) out[pos++] = '"';
    for (const char* p = v ? v : ""; *p && pos + 2 < out_sz; ++p) {
        if (*p == '"' || *p == '\\') {
            out[pos++] = '\\';
        }
        out[pos++] = *p;
    }
    if (pos + 1 < out_sz) out[pos++] = '"';
    if (pos < out_sz) out[pos] = 0;
}

void build_login_command(char* out, size_t out_sz) {
    const auto& s = telnet_settings_get();
    size_t pos = 0;
    const char* prefix = "Login ";
    while (*prefix && pos + 1 < out_sz) out[pos++] = *prefix++;
    append_quoted(out, out_sz, pos, s.username);
    if (pos + 1 < out_sz) out[pos++] = ' ';
    append_quoted(out, out_sz, pos, s.password);
    if (pos < out_sz) out[pos] = 0;
}

void send_login() {
    char cmd[96];
    build_login_command(cmd, sizeof(cmd));
    send_line(cmd);
    g_login_sent = true;
    set_status("TELNET login");
}

err_t on_connected(void*, tcp_pcb* pcb, err_t err) {
    if (err != ERR_OK) {
        set_status("TELNET fail");
        g_connected = false;
        g_pcb = nullptr;
        g_next_connect_us = time_us_32() + kRetryUs;
        return err;
    }
    g_pcb = pcb;
    g_connected = true;
    g_logged_in = false;
    g_login_sent = false;
    g_connected_at_us = time_us_32();
    g_next_login_us = g_connected_at_us + kLoginDelayUs;
    set_status("TELNET open");
    return ERR_OK;
}

void on_error(void*, err_t) {
    g_pcb = nullptr;
    g_connected = false;
    g_logged_in = false;
    g_login_sent = false;
    set_status("TELNET error");
    g_next_connect_us = time_us_32() + kRetryUs;
}

err_t on_sent(void*, tcp_pcb*, uint16_t) { return ERR_OK; }

void reply_telnet(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t r[3] = {a, b, c};
    send_raw_bytes(r, sizeof(r));
}

void send_ttype_is() {
    const uint8_t r[] = {IAC, SB, TEL_TTYPE, TEL_TTYPE_IS, 'P', 'I', 'C', 'O', '2', 'W', IAC, SE};
    send_raw_bytes(r, sizeof(r));
}

void telnet_negotiate_reply(const uint8_t* b, uint16_t len) {
    // MA2 uses real Telnet framing. Behave like a small Telnet client, not raw TCP.
    for (uint16_t i = 0; i < len; ++i) {
        if (b[i] != IAC || i + 1 >= len) continue;
        const uint8_t cmd = b[++i];

        if ((cmd == DO || cmd == DONT || cmd == WILL || cmd == WONT) && i + 1 < len) {
            const uint8_t opt = b[++i];
            if (cmd == DO) {
                if (opt == TEL_SGA || opt == TEL_TTYPE) reply_telnet(IAC, WILL, opt);
                else reply_telnet(IAC, WONT, opt);
            } else if (cmd == WILL) {
                if (opt == TEL_ECHO || opt == TEL_SGA) reply_telnet(IAC, DO, opt);
                else reply_telnet(IAC, DONT, opt);
            } else if (cmd == DONT) {
                reply_telnet(IAC, WONT, opt);
            } else if (cmd == WONT) {
                reply_telnet(IAC, DONT, opt);
            }
        } else if (cmd == SB) {
            // Handle terminal-type SEND: IAC SB 24 1 IAC SE
            uint16_t j = i + 1;
            if (j + 1 < len && b[j] == TEL_TTYPE && b[j + 1] == TEL_TTYPE_SEND) {
                send_ttype_is();
            }
            while (i + 1 < len && !(b[i] == IAC && b[i + 1] == SE)) ++i;
            if (i + 1 < len) ++i;
        }
    }
}

bool ascii_contains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    const size_t n = std::strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && (char)std::tolower((unsigned char)p[i]) == (char)std::tolower((unsigned char)needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

err_t on_recv(void*, tcp_pcb* pcb, pbuf* p, err_t err) {
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        close_conn();
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);

    char text[256]{};
    uint16_t pos = 0;
    for (pbuf* q = p; q && pos < sizeof(text) - 1; q = q->next) {
        const uint8_t* d = static_cast<const uint8_t*>(q->payload);
        telnet_negotiate_reply(d, q->len);
        for (uint16_t i = 0; i < q->len && pos < sizeof(text) - 1; ++i) {
            if (d[i] >= 32 && d[i] < 127) text[pos++] = (char)d[i];
            else if (d[i] == '\n' || d[i] == '\r') text[pos++] = ' ';
        }
    }
    text[pos] = 0;
    pbuf_free(p);

    // MA2 does not ask for username/password like a router. It expects a command:
    // Login "User" "Password"
    if (!g_login_sent && (ascii_contains(text, "login") || ascii_contains(text, "grandma") || ascii_contains(text, ">"))) {
        send_login();
    }
    if (g_login_sent && (ascii_contains(text, ">") || ascii_contains(text, "logged") || ascii_contains(text, "ok") || ascii_contains(text, "grandma"))) {
        g_logged_in = true;
        set_status("TELNET ready");
    }
    return ERR_OK;
}

void start_connect() {
    if (g_pcb || g_connected) return;
    const auto& s = telnet_settings_get();
    ip4_addr_t target;
    IP4_ADDR(&target, s.ma2_ip[0], s.ma2_ip[1], s.ma2_ip[2], s.ma2_ip[3]);
    g_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!g_pcb) {
        set_status("TELNET no pcb");
        g_next_connect_us = time_us_32() + kRetryUs;
        return;
    }
    tcp_arg(g_pcb, nullptr);
    tcp_recv(g_pcb, on_recv);
    tcp_sent(g_pcb, on_sent);
    tcp_err(g_pcb, on_error);
    set_status("TELNET connect");
    const err_t e = tcp_connect(g_pcb, &target, s.ma2_port, on_connected);
    if (e != ERR_OK) {
        tcp_abort(g_pcb);
        g_pcb = nullptr;
        set_status("TELNET wait");
        g_next_connect_us = time_us_32() + kRetryUs;
    }
}

uint8_t speed_for_abs(int v) {
    const auto& s = telnet_settings_get();
    const int pct = (v * 100) / 127;
    if (pct < s.deadzone_percent) return 0;
    if (pct < s.speed1_percent) return 1;
    if (pct < s.speed2_percent) return 2;
    return 3;
}

uint32_t interval_for_speed(uint8_t speed) {
    if (speed < 1 || speed > 3) return 0xFFFFFFFFu;
    const auto& s = telnet_settings_get();
    return (uint32_t)s.rate_ms[speed - 1] * 1000u;
}

uint16_t step_x10_for_speed(uint8_t speed) {
    if (speed < 1 || speed > 3) return 0;
    const auto& s = telnet_settings_get();
    return s.step_x10[speed - 1];
}

void format_step(uint16_t x10, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (x10 % 10 == 0) std::snprintf(out, out_sz, "%u", (unsigned)(x10 / 10));
    else std::snprintf(out, out_sz, "%u.%u", (unsigned)(x10 / 10), (unsigned)(x10 % 10));
}

void maybe_send_dir(uint8_t dir, uint8_t speed) {
    if (speed == 0 || dir >= 4) return;
    const uint32_t now = time_us_32();
    const uint32_t gap = interval_for_speed(speed);
    if (g_last_dir_speed[dir] != speed || g_last_dir_send_us[dir] == 0 || (uint32_t)(now - g_last_dir_send_us[dir]) >= gap) {
        char step[12];
        format_step(step_x10_for_speed(speed), step, sizeof(step));
        char cmd[96];
        switch (dir) {
            case 0: std::snprintf(cmd, sizeof(cmd), "Attribute \"Pan\" At - %s If Selection", step); break;
            case 1: std::snprintf(cmd, sizeof(cmd), "Attribute \"Pan\" At + %s If Selection", step); break;
            case 2: std::snprintf(cmd, sizeof(cmd), "Attribute \"Tilt\" At + %s If Selection", step); break;
            default: std::snprintf(cmd, sizeof(cmd), "Attribute \"Tilt\" At - %s If Selection", step); break;
        }
        ma2_telnet_send_command(cmd);
        g_last_dir_send_us[dir] = now;
        g_last_dir_speed[dir] = speed;
    }
}
}

void ma2_telnet_init() {
    set_status("TELNET boot");
    g_next_connect_us = time_us_32() + 1000000;
}

void ma2_telnet_tick() {
    const uint32_t now = time_us_32();
    if (!g_connected && !g_pcb && (int32_t)(now - g_next_connect_us) >= 0) start_connect();
    if (g_connected && !g_login_sent && (int32_t)(now - g_next_login_us) >= 0) send_login();
    if (g_connected && g_login_sent && !g_logged_in && (uint32_t)(now - g_connected_at_us) > kLoginAssumeReadyUs) {
        // MA2 feedback varies; after sending Login, allow commands unless socket closes.
        g_logged_in = true;
        set_status("TELNET ready");
    }
}

void ma2_telnet_reconfigure() { close_conn(); g_next_connect_us = time_us_32() + 300000; }

bool ma2_telnet_connected() { return g_connected; }
bool ma2_telnet_logged_in() { return g_connected && g_logged_in; }
const char* ma2_telnet_status() { return g_status; }

void ma2_telnet_send_command(const char* cmd) {
    if (!cmd || !g_connected || !g_pcb) return;
    if (!g_login_sent) {
        send_login();
        return;
    }
    if (!g_logged_in) return;
    send_line(cmd);
    set_status("TELNET cmd");
}


void reset_nav_repeat() {
    g_nav_prev_dpad = 8;
    g_nav_repeat_dir = 8;
    g_nav_first_us = 0;
    g_nav_last_us = 0;
}

void send_dpad_nav_cmd(uint8_t dpad) {
    // Normal/live mode:
    // Right/Left move MA2 selection with Next/Previous.
    // Up/Down stay as MA2 key navigation.
    switch (dpad) {
        case 0: ma2_telnet_send_command("Key Up"); break;
        case 2: ma2_telnet_send_command("Next"); break;
        case 4: ma2_telnet_send_command("Key Down"); break;
        case 6: ma2_telnet_send_command("Previous"); break;
        default: break;
    }
}

void process_dpad_nav(uint8_t dpad) {
    constexpr uint32_t kHoldDelayUs = 350000;
    constexpr uint32_t kRepeatUs = 90000;
    const uint32_t now = time_us_32();
    const bool is_cardinal = (dpad == 0 || dpad == 2 || dpad == 4 || dpad == 6);

    if (!is_cardinal) {
        reset_nav_repeat();
        return;
    }

    if (dpad != g_nav_prev_dpad) {
        send_dpad_nav_cmd(dpad);
        g_nav_first_us = now;
        g_nav_last_us = now;
        g_nav_repeat_dir = dpad;
    } else if (dpad == g_nav_repeat_dir) {
        if ((uint32_t)(now - g_nav_first_us) >= kHoldDelayUs &&
            (uint32_t)(now - g_nav_last_us) >= kRepeatUs) {
            send_dpad_nav_cmd(dpad);
            g_nav_last_us = now;
        }
    }
    g_nav_prev_dpad = dpad;
}

void ma2_telnet_drive_right_stick(int x, int y) {
    // x/y expected -127..127. X controls Pan. Negative Y is stick up.
    uint8_t sx = speed_for_abs(x < 0 ? -x : x);
    uint8_t sy = speed_for_abs(y < 0 ? -y : y);
    if (x < 0) maybe_send_dir(0, sx);
    else if (x > 0) maybe_send_dir(1, sx);
    if (y < 0) maybe_send_dir(2, sy);
    else if (y > 0) maybe_send_dir(3, sy);
}


void ma2_remote_process_report(const uint8_t report[63]) {
    if (!report) return;
    // USB-style DualSense input report: bytes 2/3 are right-stick X/Y.
    // Convert 0..255 center-at-128 to signed -127..127.
    int x = (int)report[2] - 128;
    int y = (int)report[3] - 128;
    if (x < -127) x = -127;
    if (x > 127) x = 127;
    if (y < -127) y = -127;
    if (y > 127) y = 127;
    ma2_telnet_drive_right_stick(x, y);

    const uint8_t dpad = report[7] & 0x0F;
    if (oled_settings_mode_active()) reset_nav_repeat();
    else process_dpad_nav(dpad);
}

void ma2_remote_tick() {
    // Right-stick repeat timing is handled in ma2_remote_process_report()
    // using the latest incoming controller reports, so no background work is
    // needed here yet. Kept as a public hook for main.cpp.
}
