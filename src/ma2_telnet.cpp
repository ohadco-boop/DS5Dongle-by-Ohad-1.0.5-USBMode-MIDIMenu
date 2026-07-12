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
uint32_t g_connect_started_us = 0;
uint32_t g_login_sent_at_us = 0;
uint32_t g_next_connect_us = 0;
uint32_t g_next_login_us = 0;
uint32_t g_next_login_retry_us = 0;
uint8_t g_login_retries = 0;
char g_status[32] = "TELNET idle";
uint32_t g_cmd_status_until_us = 0;
uint32_t g_last_tx_us = 0;
bool g_drop_pending = false;
char g_drop_status[32] = "TELNET retry";

uint32_t g_last_dir_send_us[6] = {0, 0, 0, 0, 0, 0};
uint8_t g_last_dir_speed[6] = {0, 0, 0, 0, 0, 0};
bool g_prev_buttons[MA2_BTN_COUNT]{};
uint8_t g_nav_prev_dpad = 8;
uint8_t g_nav_repeat_dir = 8;
uint32_t g_nav_first_us = 0;
uint32_t g_nav_last_us = 0;

constexpr uint32_t kRetryUs = 2000000;
constexpr uint32_t kLoginDelayUs = 250000;
constexpr uint32_t kLoginAssumeReadyUs = 900000;
constexpr uint32_t kCmdStatusUs = 250000;
constexpr uint32_t kHeartbeatUs = 25000000;
constexpr uint32_t kFastRetryUs = 300000;
constexpr uint32_t kConnectTimeoutUs = 5000000;
constexpr uint32_t kLoginRetryUs = 1000000;
constexpr uint32_t kLoginDropUs = 6000000;

constexpr uint8_t IAC  = 255;
constexpr uint8_t DONT = 254;
constexpr uint8_t DO   = 253;
constexpr uint8_t WONT = 252;
constexpr uint8_t WILL = 251;
constexpr uint8_t SB   = 250;
constexpr uint8_t SE   = 240;
constexpr uint8_t NOP  = 241;
constexpr uint8_t TEL_ECHO = 1;
constexpr uint8_t TEL_SGA  = 3;
constexpr uint8_t TEL_TTYPE = 24;
constexpr uint8_t TEL_TTYPE_IS = 0;
constexpr uint8_t TEL_TTYPE_SEND = 1;

struct HardkeyDef { const char* label; const char* cmd; };

const char* const kButtonLabels[MA2_BTN_COUNT] = {
    "DPad Up", "DPad Right", "DPad Down", "DPad Left",
    "Square", "Cross", "Circle", "Triangle",
    "L1", "R1", "L2", "R2", "Create", "Options", "L3", "R3", "PS", "Touchpad"
};

const HardkeyDef kHardkeys[MA2_HK_COUNT] = {
    {"Off", ""},
    {"Up", "Key Up"},
    {"Down", "Key Down"},
    {"Left", "Key Left"},
    {"Right", "Key Right"},
    {"Next", "Next"},
    {"Previous", "Previous"},
    {"Go+", "Go+"},
    {"Go-", "Go-"},
    {"Pause", "Pause"},
    {"Top", "Top"},
    {"On", "On"},
    {"OffKey", "Off"},
    {"Clear", "Clear"},
    {"Store", "Store"},
    {"Update", "Update"},
    {"Delete", "Delete"},
    {"Edit", "Edit"},
    {"Assign", "Assign"},
    {"Move", "Move"},
    {"Copy", "Copy"},
    {"Fixture", "Fixture"},
    {"Channel", "Channel"},
    {"Group", "Group"},
    {"Preset", "Preset"},
    {"Sequence", "Sequence"},
    {"Cue", "Cue"},
    {"Executor", "Executor"},
    {"Page", "Page"},
    {"Select", "Select"},
    {"Temp", "Temp"},
    {"Highlight", "Highlight"},
    {"Solo", "Solo"},
    {"Blackout", "Blackout"},
    {"Freeze", "Freeze"},
    {"Esc", "Key Esc"},
    {"Please", "Please"},
    {"MA", "Key MA"},
    {"Oops", "Oops"},
};

void set_status(const char* s) { std::snprintf(g_status, sizeof(g_status), "%s", s); }

void request_drop(const char* status) {
    std::snprintf(g_drop_status, sizeof(g_drop_status), "%s", status ? status : "TELNET retry");
    g_connected = false;
    g_logged_in = false;
    g_login_sent = false;
    g_login_sent_at_us = 0;
    g_next_login_retry_us = 0;
    g_login_retries = 0;
    g_cmd_status_until_us = 0;
    g_drop_pending = true;
    set_status(g_drop_status);
}

void abort_pending_conn() {
    if (g_pcb) {
        tcp_arg(g_pcb, nullptr);
        tcp_recv(g_pcb, nullptr);
        tcp_sent(g_pcb, nullptr);
        tcp_err(g_pcb, nullptr);
        tcp_abort(g_pcb);
        g_pcb = nullptr;
    }
    g_connected = false;
    g_logged_in = false;
    g_login_sent = false;
    g_login_sent_at_us = 0;
    g_next_login_retry_us = 0;
    g_login_retries = 0;
    g_cmd_status_until_us = 0;
    g_last_tx_us = 0;
    g_drop_pending = false;
    set_status(g_drop_status);
    g_next_connect_us = time_us_32() + kFastRetryUs;
}

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
    g_login_sent_at_us = 0;
    g_next_login_retry_us = 0;
    g_login_retries = 0;
    g_cmd_status_until_us = 0;
    g_last_tx_us = 0;
    g_drop_pending = false;
    set_status("TELNET closed");
    g_next_connect_us = time_us_32() + kRetryUs;
}

bool send_checked_bytes(const uint8_t* data, uint16_t len) {
    if (!g_pcb || !data || len == 0 || g_drop_pending) return false;
    if (tcp_sndbuf(g_pcb) < len) {
        request_drop("TELNET retry");
        return false;
    }
    err_t e = tcp_write(g_pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) {
        request_drop("TELNET retry");
        return false;
    }
    e = tcp_output(g_pcb);
    if (e != ERR_OK) {
        request_drop("TELNET retry");
        return false;
    }
    g_last_tx_us = time_us_32();
    return true;
}

bool send_line_checked(const char* line) {
    if (!line) return false;
    const size_t len = std::strlen(line);
    if (len + 2 > 159) return false;
    char buf[160];
    std::memcpy(buf, line, len);
    buf[len] = '\r';
    buf[len + 1] = '\n';
    return send_checked_bytes(reinterpret_cast<const uint8_t*>(buf), (uint16_t)(len + 2));
}

void send_raw_bytes(const uint8_t* data, uint16_t len) {
    if (!g_pcb || !data || len == 0) return;
    if (tcp_sndbuf(g_pcb) < len) return;
    tcp_write(g_pcb, data, len, TCP_WRITE_FLAG_COPY);
    tcp_output(g_pcb);
    g_last_tx_us = time_us_32();
}

void send_raw(const char* text) {
    if (!g_pcb || !text) return;
    const size_t len = std::strlen(text);
    if (len == 0) return;
    if (tcp_sndbuf(g_pcb) < len) return;
    tcp_write(g_pcb, text, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    tcp_output(g_pcb);
    g_last_tx_us = time_us_32();
}

bool send_line(const char* line) {
    return send_line_checked(line ? line : "");
}

bool send_telnet_nop() {
    const uint8_t nop[2] = {IAC, NOP};
    return send_checked_bytes(nop, sizeof(nop));
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

bool send_login() {
    char cmd[96];
    build_login_command(cmd, sizeof(cmd));
    if (!send_line(cmd)) {
        request_drop("TELNET retry");
        return false;
    }
    g_login_sent = true;
    g_login_sent_at_us = time_us_32();
    g_next_login_retry_us = g_login_sent_at_us + kLoginRetryUs;
    if (g_login_retries < 255) ++g_login_retries;
    set_status("TELNET login");
    return true;
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
    g_login_sent_at_us = 0;
    g_next_login_retry_us = 0;
    g_login_retries = 0;
    g_last_tx_us = g_connected_at_us;
    g_cmd_status_until_us = 0;
    g_drop_pending = false;
    set_status("TELNET open");
    return ERR_OK;
}

void on_error(void*, err_t) {
    g_pcb = nullptr;
    g_connected = false;
    g_logged_in = false;
    g_login_sent = false;
    g_login_sent_at_us = 0;
    g_next_login_retry_us = 0;
    g_login_retries = 0;
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
    // Any data from MA2 proves the connection is alive.

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
        g_login_retries = 0;
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
    tcp_nagle_disable(g_pcb);
    g_connect_started_us = time_us_32();
    set_status("TELNET connect");
    const err_t e = tcp_connect(g_pcb, &target, s.ma2_port, on_connected);
    if (e != ERR_OK) {
        tcp_abort(g_pcb);
        g_pcb = nullptr;
        g_connect_started_us = 0;
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
    if (speed == 0 || dir >= 6) return;
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
            case 3: std::snprintf(cmd, sizeof(cmd), "Attribute \"Tilt\" At - %s If Selection", step); break;
            case 4: std::snprintf(cmd, sizeof(cmd), "Attribute \"Dim\" At + %s If Selection", step); break;
            default: std::snprintf(cmd, sizeof(cmd), "Attribute \"Dim\" At - %s If Selection", step); break;
        }
        ma2_telnet_send_command(cmd);
        g_last_dir_send_us[dir] = now;
        g_last_dir_speed[dir] = speed;
    }
}

const char* hardkey_command(uint8_t hardkey) {
    if (hardkey >= MA2_HK_COUNT) return "";
    return kHardkeys[hardkey].cmd;
}

void send_mapped_button(uint8_t slot) {
    if (slot >= MA2_BTN_COUNT) return;
    const auto& s = telnet_settings_get();
    const uint8_t hk = s.button_map[slot];
    const char* c = hardkey_command(hk);
    if (c && c[0]) ma2_telnet_send_command(c);
}
}

const char* ma2_button_label(uint8_t slot) {
    if (slot >= MA2_BTN_COUNT) return "?";
    return kButtonLabels[slot];
}

const char* ma2_hardkey_label(uint8_t hardkey) {
    if (hardkey >= MA2_HK_COUNT) return "?";
    return kHardkeys[hardkey].label;
}

uint8_t ma2_hardkey_count() { return MA2_HK_COUNT; }

void ma2_telnet_init() {
    set_status("TELNET boot");
    g_next_connect_us = time_us_32() + 1000000;
}

void ma2_telnet_tick() {
    const uint32_t now = time_us_32();

    if (g_drop_pending) {
        abort_pending_conn();
        return;
    }

    // If a TCP connect attempt gets stuck, abort it and try again.
    if (g_pcb && !g_connected && g_connect_started_us &&
        (uint32_t)(now - g_connect_started_us) >= kConnectTimeoutUs) {
        request_drop("TELNET retry");
        return;
    }

    if (!g_connected && !g_pcb && (int32_t)(now - g_next_connect_us) >= 0) start_connect();

    if (g_connected && !g_login_sent && (int32_t)(now - g_next_login_us) >= 0) {
        send_login();
    }

    // Some MA2 builds do not reply consistently after Login. Retry Login a few times,
    // then recycle the socket instead of staying forever on T:---- / TELNET retry.
    if (g_connected && g_login_sent && !g_logged_in) {
        if ((uint32_t)(now - g_connected_at_us) >= kLoginDropUs) {
            request_drop("TELNET retry");
            return;
        }
        if ((int32_t)(now - g_next_login_retry_us) >= 0 && g_login_retries < 3) {
            send_login();
        }
        if ((uint32_t)(now - g_connected_at_us) > kLoginAssumeReadyUs) {
            // MA2 feedback varies; after sending Login, allow commands unless socket closes.
            g_logged_in = true;
            set_status("TELNET ready");
        }
    }

    if (g_cmd_status_until_us && g_connected && g_logged_in &&
        (int32_t)(now - g_cmd_status_until_us) >= 0 &&
        std::strcmp(g_status, "TELNET cmd") == 0) {
        g_cmd_status_until_us = 0;
        set_status("TELNET ready");
    }

    if (g_connected && g_logged_in && !g_drop_pending &&
        (g_last_tx_us == 0 || (uint32_t)(now - g_last_tx_us) >= kHeartbeatUs)) {
        // Real Telnet NOP keep-alive: keeps TCP active without entering an empty MA2 command.
        if (!send_telnet_nop()) request_drop("TELNET retry");
    }
}

void ma2_telnet_reconfigure() { close_conn(); g_next_connect_us = time_us_32() + 300000; }

bool ma2_telnet_connected() { return g_connected; }
bool ma2_telnet_logged_in() { return g_connected && g_logged_in; }
const char* ma2_telnet_status() { return g_status; }

void ma2_telnet_send_command(const char* cmd) {
    if (!cmd || !cmd[0] || g_drop_pending) return;
    if (!g_connected || !g_pcb) {
        // Start reconnect immediately; do not wait for a physical replug.
        g_next_connect_us = time_us_32();
        set_status("TELNET retry");
        return;
    }
    if (!g_login_sent) {
        send_login();
        return;
    }
    if (!g_logged_in) {
        const uint32_t now = time_us_32();
        if ((int32_t)(now - g_next_login_retry_us) >= 0) send_login();
        return;
    }
    if (!send_line(cmd)) {
        request_drop("TELNET retry");
        return;
    }
    set_status("TELNET cmd");
    g_cmd_status_until_us = time_us_32() + kCmdStatusUs;
}


void reset_nav_repeat() {
    g_nav_prev_dpad = 8;
    g_nav_repeat_dir = 8;
    g_nav_first_us = 0;
    g_nav_last_us = 0;
}

void send_dpad_nav_cmd(uint8_t dpad) {
    // Normal/live mode D-Pad is also user-mappable.
    switch (dpad) {
        case 0: send_mapped_button(MA2_BTN_DPAD_UP); break;
        case 2: send_mapped_button(MA2_BTN_DPAD_RIGHT); break;
        case 4: send_mapped_button(MA2_BTN_DPAD_DOWN); break;
        case 6: send_mapped_button(MA2_BTN_DPAD_LEFT); break;
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

void ma2_telnet_drive_left_stick_dimmer(int y) {
    // Left stick Y controls dimmer. Stick up increases Dim, down decreases.
    uint8_t sy = speed_for_abs(y < 0 ? -y : y);
    if (y < 0) maybe_send_dir(4, sy);
    else if (y > 0) maybe_send_dir(5, sy);
}

bool report_button_down(const uint8_t report[63], uint8_t slot) {
    switch (slot) {
        case MA2_BTN_SQUARE:   return (report[7] & 0x10) != 0;
        case MA2_BTN_CROSS:    return (report[7] & 0x20) != 0;
        case MA2_BTN_CIRCLE:   return (report[7] & 0x40) != 0;
        case MA2_BTN_TRIANGLE: return (report[7] & 0x80) != 0;
        case MA2_BTN_L1:       return (report[8] & 0x01) != 0;
        case MA2_BTN_R1:       return (report[8] & 0x02) != 0;
        case MA2_BTN_L2:       return (report[8] & 0x04) != 0;
        case MA2_BTN_R2:       return (report[8] & 0x08) != 0;
        case MA2_BTN_CREATE:   return (report[8] & 0x10) != 0;
        case MA2_BTN_OPTIONS:  return (report[8] & 0x20) != 0;
        case MA2_BTN_L3:       return (report[8] & 0x40) != 0;
        case MA2_BTN_R3:       return (report[8] & 0x80) != 0;
        case MA2_BTN_PS:       return (report[9] & 0x01) != 0;
        case MA2_BTN_TOUCHPAD: return (report[9] & 0x02) != 0;
        default: return false;
    }
}

void button_edge(uint8_t slot, bool down) {
    if (slot >= MA2_BTN_COUNT) return;
    if (down && !g_prev_buttons[slot]) send_mapped_button(slot);
    g_prev_buttons[slot] = down;
}

void process_non_dpad_buttons(const uint8_t report[63]) {
    button_edge(MA2_BTN_SQUARE,   (report[7] & 0x10) != 0);
    button_edge(MA2_BTN_CROSS,    (report[7] & 0x20) != 0);
    button_edge(MA2_BTN_CIRCLE,   (report[7] & 0x40) != 0);
    button_edge(MA2_BTN_TRIANGLE, (report[7] & 0x80) != 0);

    button_edge(MA2_BTN_L1,       (report[8] & 0x01) != 0);
    button_edge(MA2_BTN_R1,       (report[8] & 0x02) != 0);
    button_edge(MA2_BTN_L2,       (report[8] & 0x04) != 0);
    button_edge(MA2_BTN_R2,       (report[8] & 0x08) != 0);
    button_edge(MA2_BTN_CREATE,   (report[8] & 0x10) != 0);
    button_edge(MA2_BTN_OPTIONS,  (report[8] & 0x20) != 0);
    button_edge(MA2_BTN_L3,       (report[8] & 0x40) != 0);
    button_edge(MA2_BTN_R3,       (report[8] & 0x80) != 0);

    button_edge(MA2_BTN_PS,       (report[9] & 0x01) != 0);
    button_edge(MA2_BTN_TOUCHPAD, (report[9] & 0x02) != 0);
}

void reset_button_edges() {
    for (int i = 0; i < MA2_BTN_COUNT; ++i) g_prev_buttons[i] = false;
}

void sync_button_edges_from_report(const uint8_t report[63]) {
    if (!report) return;
    for (int i = 0; i < MA2_BTN_COUNT; ++i) g_prev_buttons[i] = report_button_down(report, (uint8_t)i);
}


void ma2_remote_process_report(const uint8_t report[63]) {
    if (!report) return;
    // USB-style DualSense input report: bytes 0/1 are left stick, bytes 2/3 are right stick.
    // Convert 0..255 center-at-128 to signed -127..127.
    int ly = (int)report[1] - 128;
    int rx = (int)report[2] - 128;
    int ry = (int)report[3] - 128;
    if (ly < -127) ly = -127; if (ly > 127) ly = 127;
    if (rx < -127) rx = -127; if (rx > 127) rx = 127;
    if (ry < -127) ry = -127; if (ry > 127) ry = 127;

    const uint8_t dpad = report[7] & 0x0F;
    if (oled_settings_mode_active()) {
        reset_nav_repeat();
        sync_button_edges_from_report(report);
        return;
    }

    ma2_telnet_drive_right_stick(rx, ry);
    ma2_telnet_drive_left_stick_dimmer(ly);
    process_dpad_nav(dpad);
    process_non_dpad_buttons(report);
}

void ma2_remote_tick() {
    // Right-stick repeat timing is handled in ma2_remote_process_report()
    // using the latest incoming controller reports, so no background work is
    // needed here yet. Kept as a public hook for main.cpp.
}
