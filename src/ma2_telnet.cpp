#include "ma2_telnet.h"
#include "telnet_settings.h"
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
bool g_sent_user = false;
bool g_sent_pass = false;
uint32_t g_connected_at_us = 0;
uint32_t g_next_connect_us = 0;
char g_status[32] = "TELNET idle";

uint32_t g_last_dir_send_us[4] = {0, 0, 0, 0};
uint8_t g_last_dir_speed[4] = {0, 0, 0, 0};

constexpr uint32_t kRetryUs = 2000000;
constexpr uint32_t kLoginGraceUs = 1500000;

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
    g_sent_user = false;
    g_sent_pass = false;
    set_status("TELNET closed");
    g_next_connect_us = time_us_32() + kRetryUs;
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
    g_sent_user = false;
    g_sent_pass = false;
    g_connected_at_us = time_us_32();
    set_status("TELNET open");
    return ERR_OK;
}

void on_error(void*, err_t) {
    g_pcb = nullptr;
    g_connected = false;
    g_logged_in = false;
    g_sent_user = false;
    g_sent_pass = false;
    set_status("TELNET error");
    g_next_connect_us = time_us_32() + kRetryUs;
}

err_t on_sent(void*, tcp_pcb*, uint16_t) { return ERR_OK; }

void telnet_negotiate_reply(const uint8_t* b, uint16_t len) {
    // Minimal Telnet option handling: refuse all options so MA2 sees plain text.
    for (uint16_t i = 0; i + 2 < len; ++i) {
        if (b[i] != 0xFF) continue;
        const uint8_t cmd = b[i + 1];
        const uint8_t opt = b[i + 2];
        uint8_t reply[3] = {0xFF, 0xFC, opt}; // WONT
        if (cmd == 0xFB || cmd == 0xFC) reply[1] = 0xFE;      // WILL/WONT -> DONT
        else if (cmd == 0xFD || cmd == 0xFE) reply[1] = 0xFC; // DO/DONT -> WONT
        else continue;
        if (g_pcb && tcp_sndbuf(g_pcb) >= sizeof(reply)) {
            tcp_write(g_pcb, reply, sizeof(reply), TCP_WRITE_FLAG_COPY);
            tcp_output(g_pcb);
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

    const auto& s = telnet_settings_get();
    if ((ascii_contains(text, "login") || ascii_contains(text, "user")) && !g_sent_user && s.username[0]) {
        send_line(s.username);
        g_sent_user = true;
        set_status("TELNET user");
    } else if (ascii_contains(text, "password") && !g_sent_pass) {
        send_line(s.password);
        g_sent_pass = true;
        set_status("TELNET pass");
    } else if (ascii_contains(text, ">") || ascii_contains(text, "grandma") || ascii_contains(text, "command")) {
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
    switch (speed) {
        case 1: return 120000;
        case 2: return 70000;
        case 3: return 40000;
        default: return 0xFFFFFFFFu;
    }
}

int step_for_speed(uint8_t speed) {
    switch (speed) {
        case 1: return 1;
        case 2: return 3;
        case 3: return 10;
        default: return 0;
    }
}

void maybe_send_dir(uint8_t dir, uint8_t speed) {
    if (speed == 0 || dir >= 4) return;
    const uint32_t now = time_us_32();
    const uint32_t gap = interval_for_speed(speed);
    if (g_last_dir_speed[dir] != speed || g_last_dir_send_us[dir] == 0 || (uint32_t)(now - g_last_dir_send_us[dir]) >= gap) {
        const int step = step_for_speed(speed);
        char cmd[88];
        switch (dir) {
            case 0: std::snprintf(cmd, sizeof(cmd), "Attribute \"Pan\" At - %d If Selection", step); break;
            case 1: std::snprintf(cmd, sizeof(cmd), "Attribute \"Pan\" At + %d If Selection", step); break;
            case 2: std::snprintf(cmd, sizeof(cmd), "Attribute \"Tilt\" At + %d If Selection", step); break;
            default: std::snprintf(cmd, sizeof(cmd), "Attribute \"Tilt\" At - %d If Selection", step); break;
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
    if (!g_connected && !g_pcb && (int32_t)(time_us_32() - g_next_connect_us) >= 0) start_connect();
    if (g_connected && !g_logged_in && (uint32_t)(time_us_32() - g_connected_at_us) > kLoginGraceUs) {
        // If MA2 accepts direct commands or login text was not detected, allow commands.
        g_logged_in = true;
        set_status("TELNET ready");
    }
}

void ma2_telnet_reconfigure() { close_conn(); g_next_connect_us = time_us_32() + 300000; }
bool ma2_telnet_connected() { return g_connected; }
bool ma2_telnet_logged_in() { return g_connected && g_logged_in; }
const char* ma2_telnet_status() { return g_status; }

void ma2_telnet_send_command(const char* cmd) {
    if (!cmd || !g_pcb || !g_connected || !g_logged_in) return;
    send_line(cmd);
}

void ma2_remote_process_report(const uint8_t report[63]) {
    // Right stick: byte 2 = X, byte 3 = Y. Center around 128.
    const int rx = (int)report[2] - 128;
    const int ry = (int)report[3] - 128;
    const int ax = rx < 0 ? -rx : rx;
    const int ay = ry < 0 ? -ry : ry;
    const uint8_t sx = speed_for_abs(ax);
    const uint8_t sy = speed_for_abs(ay);

    if (sx) maybe_send_dir(rx < 0 ? 0 : 1, sx); // X left/right -> Pan -/+
    if (sy) maybe_send_dir(ry < 0 ? 2 : 3, sy); // Y up/down -> Tilt +/-
}

void ma2_remote_tick() {}
