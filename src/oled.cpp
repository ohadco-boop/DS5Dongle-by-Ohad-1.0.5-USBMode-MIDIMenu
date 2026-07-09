#include "oled.h"
#include "oled_font.h"
#include "oled_hebrew_font.h"
#include "bt.h"
#include "slots.h"
#include "audio.h"
#include "config.h"
#include "state_mgr.h"
#include "remap.h"
#include "tusb.h"

#include <cstdio>
#include <cstring>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.5"
#endif
#ifndef FIRMWARE_VERSION_SHORT
#define FIRMWARE_VERSION_SHORT "1.0.5"
#endif

// Ohad Final branding:
// - Boot splash is always one clean owner/product line.
// - Status header uses the numeric version injected by CMake.
#define OHAD_BOOT_TITLE "DS5Dongle by Ohad"

extern uint8_t interrupt_in_data[63]; // defined in main.cpp

// Mic diagnostic counters (defined in main.cpp).
extern uint32_t bt_31_packet_count();
extern uint32_t host_out02_total();
extern uint32_t host_out02_trig_allow();
extern uint32_t host_out02_to_bt();
extern uint32_t host_out02_trig_folded();
extern uint8_t  bt_31_last_byte2();
extern uint8_t  bt_31_b2_or_mask();
extern uint16_t bt_31_len_min();
extern uint16_t bt_31_len_max();
extern void     bt_31_mic_prefix(uint8_t out[6]);
extern bool     spk_active; // main.cpp: true while host USB speaker stream is open
extern volatile bool usb_mic_stream_active; // true while host USB mic stream is open

// Global (not in the anon namespace below) so state_mgr.cpp can extern it:
// true while an OLED lightbar mode or the charging pulse owns the LED, which
// tells state_update() to ignore the host's AllowLedColor writes.
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
constexpr uint32_t kFrameUs = 100000;
bool key0_prev = true;
bool key1_prev = true;
uint32_t key0_t_us = 0;
uint32_t key1_t_us = 0;
constexpr uint32_t kDebounceUs = 20000;

// Single-press latch — armed on rising edge, fired on release. KEY0 was
// previously a double-click reboot trigger; that gesture moved to the
// KEY0+KEY1 chord below because rapid forward-navigation kept tripping it.
bool key0_armed = false;

// KEY1 long-press detection (for brightness cycling)
uint32_t key1_press_us = 0;
bool key1_was_pressed = false;
constexpr uint32_t kLongPressUs = 1500000;

// KEY0 + KEY1 simultaneous hold → watchdog_reboot. 1 s hold is long enough
// to filter accidental two-button taps but short enough to feel responsive.
uint32_t chord_held_since_us = 0;
constexpr uint32_t kChordHoldUs = 1000000;

// Brightness levels (SH1107 contrast register 0x81). fixed65b expands
// this to 10 persisted levels: index 0=100%, 9=10%. KEY1 long-press
// still cycles the levels for quick access.
constexpr uint8_t kBrightLevels[] = {0xFF, 0xE5, 0xCC, 0xB2, 0x99, 0x7F, 0x66, 0x4C, 0x33, 0x19};
constexpr int kNumBrightLevels = sizeof(kBrightLevels) / sizeof(kBrightLevels[0]);
int bright_idx = 0;
uint8_t current_contrast = 0xFF;
bool oled_flip180 = false;

int brightness_percent_from_idx(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumBrightLevels) idx = kNumBrightLevels - 1;
    return 100 - idx * 10;
}

// Auto-dim / auto-off after idle. Tracks last button/input activity.
// Tier 1: Active → full brightness (bright_idx).
// Tier 2: idle > dim threshold → contrast drops to kDimContrast (deep dim).
// Tier 3: idle > off threshold → SH1107 panel turned fully off (cmd 0xAE)
//         to prevent OLED burn-in on long unattended sits.
// The two thresholds are user-configurable (Config_body.screen_dim_timeout /
// screen_off_timeout, minutes; 0 = tier disabled) — issue #5. last_activity_us
// is 64-bit µs so the full 0..250 min range is representable without the ~71 min
// wrap of time_us_32().
// kDimContrast tuned by eye: 0x10 looked like only ~10% reduction on this
// panel (contrast-vs-brightness is heavily non-linear near the bottom of
// the register range). 0x02 is visibly dim while still legible up close.
uint64_t last_activity_us = 0;
uint32_t last_input_hash = 0;
constexpr uint8_t kDimContrast = 0x01;
enum OledPowerState { OLED_ACTIVE, OLED_DIM, OLED_OFF };
OledPowerState oled_power_state = OLED_ACTIVE;
bool prev_bt_connected = false;

// Screen ordering — single source of truth. Reorder by editing this block;
// oled_loop's switch and handle_buttons' KEY1 contextual checks use these
// names, so the indices can move without touching that code.
// Order: Status -> Help -> Slots -> feature/status pages -> Settings.
constexpr int kScreenStatus    = 0;
constexpr int kScreenHelp      = 1;
constexpr int kScreenSlots     = 2;
constexpr int kScreenLightbar  = 3;
constexpr int kScreenTriggers  = 4;
constexpr int kScreenGyro      = 5;
constexpr int kScreenTouchpad  = 6;
constexpr int kScreenRssi      = 7;
constexpr int kScreenVU        = 8;
constexpr int kScreenRemap     = 9;
constexpr int kScreenSettings  = 10;
constexpr int kNumScreens      = 11;
int current_screen = 0;

// fixed65r: physical controller shortcut for OLED paging. This reads the
// raw incoming DS5 report (interrupt_in_data) before any host-facing remap,
// so Options + D-pad Left/Right keeps working even if those controls are
// remapped in the dongle menu.
uint8_t controller_nav_last_dpad = 8;

bool controller_screen_nav_combo_active() {
    if (!bt_is_connected()) return false;
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    const bool options = (interrupt_in_data[8] & 0x20) != 0; // physical Options
    return options && (dpad == 2 || dpad == 6);              // Right / Left
}


void oled_return_to_status_screen_internal() {
    current_screen = kScreenStatus;
    last_render_us = 0;
    last_activity_us = time_us_64();
    controller_nav_last_dpad = 8;
}

bool controller_screen_nav_shortcut_internal() {
    if (!bt_is_connected()) {
        controller_nav_last_dpad = 8;
        return false;
    }

    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    const bool options = (interrupt_in_data[8] & 0x20) != 0; // physical Options
    const bool active = options && (dpad == 2 || dpad == 6);

    if (active && dpad != controller_nav_last_dpad) {
        if (dpad == 2) current_screen = (current_screen + 1) % kNumScreens;
        else           current_screen = (current_screen - 1 + kNumScreens) % kNumScreens;
        last_render_us = 0;
        last_activity_us = time_us_64();
    }

    controller_nav_last_dpad = active ? dpad : 8;
    return active; // informational only; main forwards the frame to host
}

// fixed65: Lightbar has only two user-facing modes. Keep legacy numeric values
// so existing saved configs (8=HOST, 9=BATT) remain compatible.
constexpr int kLbModeHost = 8;
constexpr int kLbModeBattery = 9;
constexpr int kNumLbModes = 10;

// Settings screen state
constexpr int kNumSettingsItems = 21; // visible Settings rows; AudBuf and Pico LED stay internal/fixed for config compatibility
// Stable item IDs.  These stay mapped to the saved Config_body fields; the
// visible menu order is controlled by kSettingsOrder below.
constexpr int kSettingsHapticsGainIdx = 0;
constexpr int kSettingsSpeakerVolIdx  = 1;
constexpr int kSettingsIdleIdx        = 2;
constexpr int kSettingsPicoLedIdx     = 3;
constexpr int kSettingsPollingIdx     = 4;
constexpr int kSettingsAudioBufferIdx = 5;
constexpr int kSettingsControllerIdx  = 6;
constexpr int kSettingsAutoHapEnaIdx  = 7;
constexpr int kSettingsAutoHapGainIdx = 8;
constexpr int kSettingsAutoHapLpIdx   = 9;
constexpr int kSettingsBrightnessIdx  = 10;
constexpr int kSettingsRotationIdx    = 11;
constexpr int kSettingsMicGainIdx     = 12;
constexpr int kSettingsScrDimIdx      = 13;
constexpr int kSettingsScrOffIdx      = 14;
constexpr int kSettingsBtMicIdx       = 15;
constexpr int kSettingsCtrlWakeIdx    = 16;
constexpr int kSettingsPowerComboIdx  = 17;
constexpr int kSettingsAudioKeepIdx   = 18;
constexpr int kSettingsLanguageIdx    = 19;
constexpr int kSettingsResetIdx       = 20;
constexpr int kSettingsWipeSlotsIdx   = 21;
constexpr int kSettingsUsbModeIdx     = 22;

// User-facing order: basic UI/control first, audio/haptics together, display
// and power behavior together, destructive actions last.
constexpr int kSettingsOrder[kNumSettingsItems] = {
    kSettingsLanguageIdx,
    kSettingsUsbModeIdx,
    kSettingsControllerIdx,
    kSettingsPollingIdx,
    kSettingsIdleIdx,
    kSettingsAudioKeepIdx,

    kSettingsSpeakerVolIdx,
    kSettingsMicGainIdx,
    kSettingsBtMicIdx,

    kSettingsHapticsGainIdx,
    kSettingsAutoHapEnaIdx,
    kSettingsAutoHapGainIdx,
    kSettingsAutoHapLpIdx,

    kSettingsBrightnessIdx,
    kSettingsRotationIdx,
    kSettingsScrDimIdx,
    kSettingsScrOffIdx,
    kSettingsCtrlWakeIdx,
    kSettingsPowerComboIdx,

    kSettingsWipeSlotsIdx,
    kSettingsResetIdx,
};

static inline int settings_item_for_row(int row) { return kSettingsOrder[row]; }

Config_body settings_local{};
int settings_sel = 0;
static inline int settings_selected_item() { return settings_item_for_row(settings_sel); }
bool settings_dirty = false;
bool settings_init_done = false;
uint8_t settings_last_dpad = 8;  // 8 = released
uint8_t settings_repeat_dpad = 8;
uint32_t settings_next_repeat_us = 0;
uint8_t settings_last_face = 0;
const char* settings_save_status = "";  // shown briefly after Triangle press
uint32_t settings_save_status_until_us = 0;

// OLED Remap screen state. This replaces the old hardcoded 3-button cycle
// menu item. The table maps physical/source button -> reported/target button.
// Target 0xFF means the source button is disabled. remap_enable lives in
// Config_body so the whole table can be bypassed without erasing it.
constexpr int kRemapEnableIdx = 0;
constexpr int kRemapFirstButtonIdx = 1;
constexpr int kRemapResetIdx = 1 + kRemapCount;
constexpr int kNumRemapItems = 2 + kRemapCount;
Config_body remap_config_local{};
uint8_t remap_local[kRemapCount]{};
int remap_sel = 0;
bool remap_init_done = false;
bool remap_dirty = false;
uint8_t remap_last_dpad = 8;
uint8_t remap_repeat_dpad = 8;
uint32_t remap_next_repeat_us = 0;
uint8_t remap_last_face = 0;
const char* remap_save_status = "";
uint32_t remap_save_status_until_us = 0;
constexpr uint32_t kSaveStatusVisibleUs = 2000000u;
constexpr uint32_t kMenuRepeatStartUs = 400000; // match Help: first repeat after 0.4 s
constexpr uint32_t kMenuRepeatEveryUs = 100000; // then continue at a fast readable pace

void settings_set_save_status(const char* s) {
    settings_save_status = s ? s : "";
    settings_save_status_until_us = settings_save_status[0] ? ((uint32_t)time_us_32() + kSaveStatusVisibleUs) : 0;
}

void remap_set_save_status(const char* s) {
    remap_save_status = s ? s : "";
    remap_save_status_until_us = remap_save_status[0] ? ((uint32_t)time_us_32() + kSaveStatusVisibleUs) : 0;
}

void settings_reset_repeat() {
    settings_repeat_dpad = 8;
    settings_next_repeat_us = 0;
}

void remap_reset_repeat() {
    remap_repeat_dpad = 8;
    remap_next_repeat_us = 0;
}

static inline bool dpad_is_up_down(uint8_t dpad) {
    return dpad == 0 || dpad == 4;
}

void settings_scroll_step(uint8_t dpad) {
    if (dpad == 0) settings_sel = (settings_sel - 1 + kNumSettingsItems) % kNumSettingsItems;
    else if (dpad == 4) settings_sel = (settings_sel + 1) % kNumSettingsItems;
}

void remap_scroll_step(uint8_t dpad) {
    if (dpad == 0) remap_sel = (remap_sel - 1 + kNumRemapItems) % kNumRemapItems;
    else if (dpad == 4) remap_sel = (remap_sel + 1) % kNumRemapItems;
}

void service_save_status_timeouts() {
    const uint32_t now = (uint32_t)time_us_32();
    if (settings_save_status[0] && settings_save_status_until_us && (int32_t)(now - settings_save_status_until_us) >= 0) {
        settings_save_status = "";
        settings_save_status_until_us = 0;
    }
    if (remap_save_status[0] && remap_save_status_until_us && (int32_t)(now - remap_save_status_until_us) >= 0) {
        remap_save_status = "";
        remap_save_status_until_us = 0;
    }
}

// Help screen: built-in manual-style guide. It is deliberately read-only and
// uses only controller D-pad Up/Down for scrolling, so it never changes runtime
// state or touches flash/audio paths. Holding Up/Down auto-scrolls, and the
// viewport wraps from the last help page back to the first page and vice versa.
int help_scroll = 0;
uint8_t help_last_dpad = 8;
uint8_t help_repeat_dpad = 8;
uint32_t help_next_repeat_us = 0;
constexpr uint32_t kHelpRepeatStartUs = 400000; // first repeat after 0.4 s
constexpr uint32_t kHelpRepeatEveryUs = 100000; // then one row per OLED frame


// Factory-reset hold-Triangle-2s state. Borrowed from zurce/DS5Dongle-OLED's
// "hold to wipe" UX pattern (https://github.com/zurce/DS5Dongle-OLED).
uint32_t settings_tri_press_us = 0;
bool settings_reset_triggered = false;
constexpr uint32_t kResetHoldUs = 2000000;

uint8_t lb_r = 0, lb_g = 0, lb_b = 0;

// Lightbar mode + favorite slots: 0 = LIVE tilt preview; 1..4 = saved slots F0..F3.
// These are seeded from flash (lightbar_load_config) at boot; the defaults here
// only apply before that runs. lb_dirty tracks an unsaved mode/favorite change
// so we persist once on leaving the Lightbar screen instead of per button press.
int lb_mode = kLbModeHost;
uint8_t lb_fav_r[4] = {255, 0,   0,   255}; // Red, Green, Blue, White defaults
uint8_t lb_fav_g[4] = {0,   255, 0,   255};
uint8_t lb_fav_b[4] = {0,   0,   255, 255};
uint8_t lb_last_face = 0;
bool lb_dirty = false;

uint32_t rumble_off_at_us = 0;
bool rumble_active = false;
constexpr uint32_t kRumbleBurstUs = 250000;

// Ohad fixed19: short on-screen status popup, used by the PS+Options
// controller power-off combo. Stored as a small fixed buffer so callers
// can pass string literals or temporary buffers safely.
char oled_popup_msg[64] = {0};
uint32_t oled_popup_until_us = 0;

int trigger_preset = 0;
const char* const kTrigPresetNames[] = {"Off", "Feedback", "Weapon", "Vibration", "Bow", "Gallop", "Machine"};

// Rising-edge trackers for the screens whose K1=cycle action moved to a
// controller button. Trigger Test uses △ (byte 7 bit 7); Lightbar uses R1
// (byte 8 bit 1) because △ is already taken on Lightbar for "save current
// RGB to favorite slot 0".
uint8_t triggers_last_face = 0;
uint8_t lb_last_buttons = 0;
constexpr int kNumTrigPresets = 7;

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
    b = ((b & 0x55) << 1) | ((b & 0xAA) >> 1);
    b = ((b & 0x33) << 2) | ((b & 0xCC) >> 2);
    b = ((b & 0x0F) << 4) | ((b & 0xF0) >> 4);
    return b;
}

void hw_reset() {
    gpio_put(kPinRST, 1); sleep_ms(100);
    gpio_put(kPinRST, 0); sleep_ms(100);
    gpio_put(kPinRST, 1); sleep_ms(100);
}

void sh1107_set_contrast(uint8_t value) {
    if (value == current_contrast) return;
    current_contrast = value;
    cmd(0x81); cmd(value);
}

void sh1107_init() {
    cmd(0xAE);
    cmd(0x00); cmd(0x10);
    cmd(0xB0);
    cmd(0xDC); cmd(0x00);
    cmd(0x81); cmd(0x6F);
    current_contrast = 0x6F;
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

// Forward-declared so flush_fb can paint the per-button arrows on top of
// the rendered framebuffer just before SPI sends it to the OLED. Body
// lives near the other text-drawing helpers below.
void draw_button_chrome();

void flush_fb_raw() {
    cmd(0xB0);
    for (int j = 0; j < kH; j++) {
        const uint8_t col = kH - 1 - j;
        cmd(0x00 + (col & 0x0F));
        cmd(0x10 + (col >> 4));
        if (!oled_flip180) {
            for (int i = 0; i < kRowBytes; i++) {
                data_byte(reverse_byte(fb[j * kRowBytes + i]));
            }
        } else {
            // Software 180° rotation: display row j gets framebuffer row 63-j,
            // with X mirrored. Normal flush already reverses bits inside each
            // byte, so sending the source bytes in reverse order without
            // reverse_byte() gives a true full-frame 180° flip.
            const int src_y = kH - 1 - j;
            for (int i = 0; i < kRowBytes; i++) {
                data_byte(fb[src_y * kRowBytes + (kRowBytes - 1 - i)]);
            }
        }
    }
}

void flush_fb() {
    draw_button_chrome();
    flush_fb_raw();
}

void fb_clear() { memset(fb, 0, sizeof(fb)); }

void px(int x, int y, bool on) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    uint8_t *p = &fb[y * kRowBytes + (x / 8)];
    uint8_t m = 1 << (7 - (x % 8));
    if (on) *p |= m; else *p &= ~m;
}

void rect_outline(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) { px(x + i, y, true); px(x + i, y + h - 1, true); }
    for (int i = 0; i < h; i++) { px(x, y + i, true); px(x + w - 1, y + i, true); }
}

void rect_filled(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(x + i, y + j, true);
}

// XOR-invert every pixel in a region (used to flash a control "pressed").
void rect_invert(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int xx = x + i, yy = y + j;
            if (xx < 0 || xx >= kW || yy < 0 || yy >= kH) continue;
            fb[yy * kRowBytes + (xx / 8)] ^= 1 << (7 - (xx % 8));
        }
}

void draw_char(int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = kFont5x7[c - 0x20];
    for (int col = 0; col < kFontW; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < kFontH; row++) {
            if (bits & (1 << row)) px(x + col, y + row, true);
        }
    }
}

void draw_text(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

bool ui_hebrew() {
    return get_config().ui_language == 1;
}

uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)(*ps);
    if (s[0] < 0x80) { *ps += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        *ps += 2;
        return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *ps += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) |
               (uint32_t)(s[2] & 0x3F);
    }
    *ps += 1;
    return '?';
}

const HebrewGlyph8* hebrew_glyph(uint32_t cp) {
    for (int i = 0; i < kHebrewGlyph8Count; ++i) {
        if (kHebrewGlyph8[i].codepoint == cp) return &kHebrewGlyph8[i];
    }
    return nullptr;
}

int hebrew_char_width(uint32_t cp) {
    if (cp == ' ') return 4;
    if (cp == '-' || cp == '/' || cp == ':' || cp == '.' || cp == ',') return 4;
    if (cp >= '0' && cp <= '9') return 6;
    const HebrewGlyph8 *g = hebrew_glyph(cp);
    return g ? (int)g->width : 6;
}

int hebrew_text_width(const char *s) {
    int w = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        w += hebrew_char_width(cp) + 1;
    }
    return w > 0 ? w - 1 : 0;
}

void draw_hebrew_char_left(int x, int y, uint32_t cp) {
    if (cp == ' ') return;
    if (cp >= 0x20 && cp <= 0x7E) { draw_char(x, y, (char)cp); return; }
    const HebrewGlyph8 *g = hebrew_glyph(cp);
    if (!g) return;
    for (int row = 0; row < 8; ++row) {
        const uint8_t bits = g->rows[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (1u << (7 - col))) px(x + col, y + row, true);
        }
    }
}

bool hebrew_is_ascii_run_char(uint32_t cp) {
    // Treat spaces as neutral RTL separators, not as part of an ASCII run.
    // This keeps mixed Hebrew/English help lines from attaching the Hebrew
    // word to the English term when the run is drawn right-aligned.
    return cp >= 0x21 && cp <= 0x7E;
}

int hebrew_run_width(const uint32_t *cps, int first, int last) {
    int w = 0;
    for (int i = first; i < last; ++i) w += hebrew_char_width(cps[i]) + 1;
    return w > 0 ? w - 1 : 0;
}

// Draw a Hebrew string right-aligned. Hebrew glyph order is rendered from
// right to left; ASCII runs such as KEY1, PS+Options, HOST or AutoHap are drawn
// left-to-right inside their reserved width. Spaces stay neutral between runs so
// mixed Hebrew/English help lines keep a visible gap in the correct place.
void draw_hebrew_r(int right_x, int y, const char *s) {
    const char *p = s;
    uint32_t cps[64];
    int n = 0;
    while (*p && n < (int)(sizeof(cps) / sizeof(cps[0]))) cps[n++] = utf8_next(&p);

    int x = right_x;
    for (int i = 0; i < n;) {
        const uint32_t cp = cps[i];
        if (hebrew_is_ascii_run_char(cp)) {
            int j = i + 1;
            while (j < n && hebrew_is_ascii_run_char(cps[j])) ++j;
            const int run_w = hebrew_run_width(cps, i, j);
            x -= run_w;
            int lx = x;
            for (int k = i; k < j; ++k) {
                draw_hebrew_char_left(lx, y, cps[k]);
                lx += hebrew_char_width(cps[k]) + 1;
            }
            x -= 1;
            i = j;
        } else {
            const int w = hebrew_char_width(cp);
            x -= w;
            draw_hebrew_char_left(x, y, cp);
            x -= 1;
            ++i;
        }
    }
}

void draw_title(const char *en, const char *he) {
    if (ui_hebrew()) draw_hebrew_r(126, 0, he);
    else draw_text(6, 0, en);
}

void draw_no_controller(int x, int y) {
    if (ui_hebrew()) draw_hebrew_r(126, y, "אין שלט");
    else draw_text(x, y, "(no controller)");
}

const char* oled_status_he(const char *s) {
    if (!s || !s[0]) return "";
    if (strcmp(s, "Saved!") == 0) return "נשמר";
    if (strcmp(s, "Save pending") == 0) return "ממתין";
    if (strcmp(s, "Save FAIL") == 0) return "שגיאה";
    if (strcmp(s, "Reset pending") == 0) return "ממתין";
    if (strcmp(s, "Reset!") == 0) return "אופס";
    if (strcmp(s, "Reset FAIL") == 0) return "שגיאה";
    if (strcmp(s, "Slots wiped!") == 0) return "נמחק";
    if (strcmp(s, "USB Reboot") == 0) return "מאתחל";
    return "";
}

const char* oled_popup_he(const char *s) {
    if (!s || !s[0]) return "";
    // Full-screen splash/status popups that originate outside oled.cpp.
    // Keep these short: the SH1107 is 128x64 and Hebrew glyphs are 5-8 px wide.
    if (strcmp(s, "Powering Off...") == 0) return "מכבה שלט";
    if (strcmp(s, "Power Off") == 0) return "כיבוי שלט";
    if (strcmp(s, "Power off") == 0) return "כיבוי שלט";
    if (strcmp(s, "Pico Mic On") == 0) return "מיק שלט פעיל";
    if (strcmp(s, "Pico Mic Off") == 0) return "מיק שלט כבוי";
    if (strcmp(s, "Mic On") == 0) return "מיק פעיל";
    if (strcmp(s, "Mic Off") == 0) return "מיק כבוי";
    if (strcmp(s, "Headset Mic") == 0) return "מיק אוזניות";
    if (strcmp(s, "Headset") == 0) return "אוזניות";
    if (strcmp(s, "Headphones") == 0) return "אוזניות";
    if (strcmp(s, "Jack Out") == 0) return "אוזניות נותקו";
    if (strcmp(s, "Saved!") == 0) return "נשמר";
    if (strcmp(s, "Save pending") == 0) return "ממתין";
    if (strcmp(s, "Save FAIL") == 0) return "שגיאה";
    if (strcmp(s, "USB Reboot") == 0) return "מאתחל USB";
    return s;
}

// Small PlayStation-style button symbols.  These are drawn as pixels instead
// of font glyphs so they work in both English and Hebrew UI modes.
void draw_tri_icon(int x, int y) {
    px(x + 3, y + 0, true);
    px(x + 2, y + 1, true); px(x + 4, y + 1, true);
    px(x + 1, y + 2, true); px(x + 5, y + 2, true);
    px(x + 0, y + 3, true); px(x + 6, y + 3, true);
    for (int i = 0; i < 7; ++i) px(x + i, y + 4, true);
}

void draw_circle_icon(int x, int y) {
    px(x + 2, y + 0, true); px(x + 3, y + 0, true); px(x + 4, y + 0, true);
    px(x + 1, y + 1, true); px(x + 5, y + 1, true);
    px(x + 0, y + 2, true); px(x + 6, y + 2, true);
    px(x + 0, y + 3, true); px(x + 6, y + 3, true);
    px(x + 0, y + 4, true); px(x + 6, y + 4, true);
    px(x + 1, y + 5, true); px(x + 5, y + 5, true);
    px(x + 2, y + 6, true); px(x + 3, y + 6, true); px(x + 4, y + 6, true);
}

void draw_cross_icon(int x, int y) {
    for (int i = 0; i < 7; ++i) {
        px(x + i, y + i, true);
        px(x + 6 - i, y + i, true);
    }
}

void draw_square_icon(int x, int y) {
    rect_outline(x, y, 7, 7);
}

void draw_bullet_dot(int x, int y) {
    rect_filled(x, y + 2, 3, 3);
}

void draw_arrow_icon(int x, int y, char dir) {
    // 7x7 directional arrows for D-pad / Remap.
    if (dir == 'U') {
        px(x + 3, y + 0, true);
        px(x + 2, y + 1, true); px(x + 3, y + 1, true); px(x + 4, y + 1, true);
        px(x + 1, y + 2, true); px(x + 3, y + 2, true); px(x + 5, y + 2, true);
        for (int j = 3; j < 7; ++j) px(x + 3, y + j, true);
    } else if (dir == 'D') {
        for (int j = 0; j < 4; ++j) px(x + 3, y + j, true);
        px(x + 1, y + 4, true); px(x + 3, y + 4, true); px(x + 5, y + 4, true);
        px(x + 2, y + 5, true); px(x + 3, y + 5, true); px(x + 4, y + 5, true);
        px(x + 3, y + 6, true);
    } else if (dir == 'L') {
        px(x + 0, y + 3, true);
        px(x + 1, y + 2, true); px(x + 1, y + 3, true); px(x + 1, y + 4, true);
        px(x + 2, y + 1, true); px(x + 2, y + 3, true); px(x + 2, y + 5, true);
        for (int i = 3; i < 7; ++i) px(x + i, y + 3, true);
    } else { // 'R'
        for (int i = 0; i < 4; ++i) px(x + i, y + 3, true);
        px(x + 4, y + 1, true); px(x + 4, y + 3, true); px(x + 4, y + 5, true);
        px(x + 5, y + 2, true); px(x + 5, y + 3, true); px(x + 5, y + 4, true);
        px(x + 6, y + 3, true);
    }
}

void draw_status_icon_box(int x, int y, int w, int h, bool pressed) {
    if (pressed) rect_invert(x, y, w, h);
}

void draw_create_icon(int x, int y) {
    // DualSense Create: three small stacked strokes.
    rect_outline(x, y + 1, 7, 5);
    px(x + 2, y + 2, true); px(x + 3, y + 2, true); px(x + 4, y + 2, true);
    px(x + 2, y + 4, true); px(x + 3, y + 4, true); px(x + 4, y + 4, true);
}

void draw_options_icon(int x, int y) {
    // DualSense Options: menu lines.
    for (int i = 0; i < 7; ++i) {
        px(x + i, y + 1, true);
        px(x + i, y + 3, true);
        px(x + i, y + 5, true);
    }
}

void draw_touchpad_icon(int x, int y) {
    rect_outline(x, y, 13, 7);
    px(x + 3, y + 2, true); px(x + 9, y + 2, true);
    px(x + 6, y + 4, true);
}

void draw_mute_icon(int x, int y) {
    // Tiny microphone symbol with slash.
    rect_outline(x + 2, y, 4, 5);
    px(x + 1, y + 4, true); px(x + 6, y + 4, true);
    px(x + 3, y + 5, true); px(x + 4, y + 5, true);
    px(x + 4, y + 6, true);
    for (int i = 0; i < 7; ++i) px(x + i, y + 6 - i, true);
}

void draw_ps_text_icon(int x, int y) {
    draw_text(x, y, "PS");
}

void draw_status_text_button(int x, int y, const char *label, bool pressed) {
    draw_text(x, y, label);
    if (pressed) {
        const int w = (int)strlen(label) * 6 + 2;
        rect_invert(x - 1, y - 1, w, 9);
    }
}

void draw_tri_footer_en(int x, int y, const char *suffix) {
    draw_tri_icon(x, y + 1);
    draw_text(x + 10, y, suffix);
}

void draw_tri_footer_he_save() {
    const char *txt = "שמירה ב";
    draw_hebrew_r(126, 56, txt);
    draw_tri_icon(126 - hebrew_text_width(txt) - 10, 57);
}

void draw_tri_footer_he_hold(const char *action) {
    draw_hebrew_r(126, 56, "החזק");
    draw_tri_icon(82, 57);
    draw_hebrew_r(74, 56, action);
}

// Button-chrome strip on the left edge of every screen. KEY0 (top button)
// shows '>' at y=8; KEY1 (bottom button) shows '<' at y=49. Painted by
// flush_fb() on top of the rendered framebuffer so it never gets clobbered.
// Per-screen renderers reserve x ∈ [0..5] (5-wide glyph + 1 padding) and
// start main content at kContentX.
constexpr int kContentX = 0;
void draw_button_chrome() {}

// Pixel-art icon support. Visual approach inspired by zurce/DS5Dongle-OLED
// (https://github.com/zurce/DS5Dongle-OLED) — credit to zurce for the idea
// of decorating the OLED with small bitmaps instead of bare text/shapes.
// Bitmap layout: row-major, MSB = leftmost pixel, ceil(w/8) bytes per row.
void draw_icon(int x, int y, const uint8_t *bitmap, int w, int h) {
    const int row_bytes = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint8_t byte = bitmap[row * row_bytes + (col / 8)];
            const uint8_t mask = (uint8_t)(1u << (7 - (col % 8)));
            if (byte & mask) px(x + col, y + row, true);
        }
    }
}

// 8x8 "link active" filled circle (drawn when DS5 is paired)
static const uint8_t kIconLinkOn[8] = {
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
};
// 8x8 "link inactive" hollow circle (drawn when waiting for DS5)
static const uint8_t kIconLinkOff[8] = {
    0b00111100,
    0b01000010,
    0b10000001,
    0b10000001,
    0b10000001,
    0b10000001,
    0b01000010,
    0b00111100,
};

// Battery icon — body 52x8 + small nub on the right. Inside fill scales with pct.
void draw_battery_icon(int x, int y, int pct) {
    rect_outline(x, y, 52, 8);
    rect_filled(x + 52, y + 2, 3, 4);
    int fill = (pct * 48) / 100;
    if (fill < 0) fill = 0;
    if (fill > 48) fill = 48;
    if (fill > 0) rect_filled(x + 2, y + 2, fill, 4);
}

void send_rumble(uint8_t amplitude) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[1] = 0x00;
    pkt[2] = 0x10;
    pkt[3] = 0x03;
    pkt[5] = amplitude;
    pkt[6] = amplitude;
    bt_write(pkt, sizeof(pkt));
}

void rumble_burst_tick(uint32_t now) {
    if (rumble_active && (int32_t)(now - rumble_off_at_us) >= 0) {
        send_rumble(0);
        rumble_active = false;
    }
}

// Trigger effect param format follows dualsensectl's reverse-engineering.
// Modes 0x21/0x25/0x26 use bitpacked 10-zone arrays, not raw position bytes.
void send_trigger_effect(int preset) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[3] = 0x0C; // valid_flag0: RIGHT_TRIGGER_MOTOR_ENABLE | LEFT_TRIGGER_MOTOR_ENABLE

    uint8_t mode = 0x05; // OFF
    uint8_t p[9] = {0};

    switch (preset) {
        case 0: // Off
            mode = 0x05;
            break;
        case 1: { // Feedback — all 10 zones at max strength 8
            mode = 0x21;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            break;
        }
        case 2: { // Weapon — snap between positions 3 and 5, force 8
            mode = 0x25;
            const uint16_t start_stop = (1u << 3) | (1u << 5);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = 7; // force = strength - 1
            break;
        }
        case 3: { // Vibration — all 10 zones at amplitude 8, frequency 30 Hz
            mode = 0x26;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            p[8] = 30;
            break;
        }
        case 4: { // Bow — drawing resistance + snap at position 6
            mode = 0x22;
            const uint16_t start_stop = (1u << 2) | (1u << 6);
            const uint8_t force_pair = 7u | (7u << 3); // strength=8, snap=8
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            break;
        }
        case 5: { // Galloping
            mode = 0x23;
            const uint16_t start_stop = (1u << 0) | (1u << 9);
            const uint8_t ratio = (5u & 0x07) | ((1u & 0x07) << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = ratio;
            p[3] = 5; // frequency
            break;
        }
        case 6: { // Machine gun
            mode = 0x27;
            const uint16_t start_stop = (1u << 1) | (1u << 8);
            const uint8_t force_pair = 7u | (7u << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            p[3] = 20; // frequency
            p[4] = 0;  // period
            break;
        }
    }

    pkt[13] = mode;
    for (int i = 0; i < 9; i++) pkt[14 + i] = p[i];
    pkt[24] = mode;
    for (int i = 0; i < 9; i++) pkt[25 + i] = p[i];

    bt_write(pkt, sizeof(pkt));
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b);

void handle_buttons() {
    const uint32_t now = time_us_32();
    const bool k0 = gpio_get(kPinKey0);
    const bool k1 = gpio_get(kPinKey1);

    // KEY0 + KEY1 chord — both held >= kChordHoldUs triggers watchdog_reboot.
    // Pre-empts the per-key handlers so a chord cancels any armed single
    // press (whichever key gets released first won't also navigate).
    const bool chord = !k0 && !k1;
    if (chord) {
        if (chord_held_since_us == 0) chord_held_since_us = now;
        key0_armed = false;
        key1_was_pressed = false;
        if ((now - chord_held_since_us) >= kChordHoldUs) {
            watchdog_reboot(0, 0, 0);
        }
    } else {
        chord_held_since_us = 0;
    }

    // KEY0: arm on debounced rising edge, fire "next screen" on release.
    // Releasing without a chord during the hold = pure forward-nav.
    if (!k0 && key0_prev && (now - key0_t_us) > kDebounceUs) {
        key0_t_us = now;
        key0_armed = true;
        last_activity_us = time_us_64();
    }
    if (k0 && !key0_prev && key0_armed) {
        key0_armed = false;
        current_screen = (current_screen + 1) % kNumScreens;
        last_render_us = 0;
        last_activity_us = time_us_64();
    }

    // KEY1: arm on press, fire on release. Short press = back; long press
    // = brightness cycle (unchanged). Trigger-preset / lightbar-mode cycle
    // moved to the DualSense △ button — see triggers_handle_input() and
    // lightbar_handle_input(). The chord above clears key1_was_pressed so
    // a released-after-chord K1 doesn't navigate back.
    if (!k1 && key1_prev && (now - key1_t_us) > kDebounceUs) {
        key1_t_us = now;
        key1_press_us = now;
        key1_was_pressed = true;
        last_activity_us = time_us_64();
    }
    if (k1 && !key1_prev && key1_was_pressed) {
        key1_was_pressed = false;
        const uint32_t held = now - key1_press_us;
        last_activity_us = time_us_64();
        if (held > kLongPressUs) {
            bright_idx = (bright_idx + 1) % kNumBrightLevels;
            // Persist so the choice survives a power cycle (issue #9). Keep
            // settings_local in sync too, so a later Settings-screen save can't
            // clobber screen_brightness with its stale snapshot.
            Config_body b = get_config();
            b.screen_brightness = (uint8_t)bright_idx;
            set_config(b);
            const bool save_ok = config_save();
            settings_set_save_status(save_ok ? (config_save_pending() ? "Save pending" : "Saved!") : "Save FAIL");
            settings_local.screen_brightness = (uint8_t)bright_idx;
        } else {
            current_screen = (current_screen - 1 + kNumScreens) % kNumScreens;
            last_render_us = 0;
        }
    }

    key0_prev = k0;
    key1_prev = k1;
}

// --- Charge ETA tracker --------------------------------------------------
// The DS5 only reports battery in 10% steps (interrupt_in_data[52] low
// nibble, 0..10; high nibble is power-state, 1 == charging). We can't read a
// finer percentage over BT, so a smooth countdown is impossible. Instead we
// time how long each 10% step takes while charging and extrapolate the
// remaining steps. Sampled once per frame from oled_loop (continuously, so
// the estimate stays current even while the panel is dimmed/off and even when
// the user is on another screen); render_screen reads g_charge_eta.
//
// Taper correction: Li-ion CC/CV charging slows sharply near the top, so a
// flat "time per step × steps left" runs optimistic in the last ~20%. Each
// measured step is normalised to a bulk-equivalent duration (divide out the
// step's taper weight); the remaining steps are then re-weighted. This makes
// the estimate consistent whether the user plugged in near-empty or near-full.
struct ChargeEta {
    bool charging;    // pstate == 1 (so the token shows only while charging)
    bool valid;       // minutes is meaningful (provisional or measured)
    bool provisional; // true until a full step is timed — using the default rate
    int  minutes;     // estimated minutes to 100%
};
ChargeEta g_charge_eta{};

// Default bulk-step duration used for a provisional estimate before any real
// step has been timed, so the token shows "~Nm?" immediately on plug-in instead
// of sitting on "~--m" for ~15-20 min. Tuned to an observed ~15 min per 10% on
// this dongle's charge current; it self-corrects to the measured rate (and drops
// the "?") as soon as the first clean step completes.
constexpr float kDefaultStepUs = 15.0f * 60.0f * 1000000.0f;

// Ceiling on a single timed step's bulk-equivalent duration. A genuine idle 10%
// step on this dongle is ~15 min; anything past ~30 min is almost always an
// anomalous/under-load sample (e.g. the controller in use while charging, or a
// battery-nibble bounce) that would otherwise balloon the projection — observed
// reading ~222m at 70% off one ~47-min step. We clamp such samples instead of
// trusting them, and pair that with a median over kRing steps so one bad reading
// can't dominate the estimate.
constexpr float kMaxStepUs = 30.0f * 60.0f * 1000000.0f;

// Relative time the step *ending* at `to_level` (10% units, 1..10) takes vs a
// bulk step. Tuned to the Li-ion CV taper: ~80% onward stretches out.
static float charge_step_weight(int to_level) {
    if (to_level >= 10) return 2.2f;  // 90→100% (constant-voltage tail)
    if (to_level == 9)  return 1.5f;  // 80→90%  (taper begins)
    return 1.0f;                      // bulk constant-current region
}

void sample_charge_eta() {
    constexpr int kRing = 5;                 // median over the last few steps
    static float    ring[kRing] = {0};       // bulk-equivalent step durations (us)
    static int      ring_count = 0;
    static int      ring_head = 0;
    static int      cur_step = -1;           // last observed 10% step
    static uint64_t step_start_us = 0;
    static bool     was_charging = false;
    static bool     first_step_pending = false;  // discard the partial step at plug-in

    const uint8_t pwr   = interrupt_in_data[52];
    int           step  = pwr & 0x0F;
    if (step > 10) step = 10;
    const uint8_t pstate = pwr >> 4;
    const bool charging = bt_is_connected() && (pstate == 1);

    if (!charging) {
        g_charge_eta = ChargeEta{};          // clears charging/valid/minutes
        ring_count = ring_head = 0;
        cur_step = -1;
        was_charging = false;
        return;
    }

    const uint64_t now = time_us_64();
    if (!was_charging) {
        // Just plugged in: start timing from here. The step in progress is
        // partial, so its duration gets discarded when it completes.
        cur_step = step;
        step_start_us = now;
        ring_count = ring_head = 0;
        first_step_pending = true;
        was_charging = true;
    } else if (step == cur_step + 1) {
        // One clean step completed. Skip the first (partial) one; otherwise
        // record its bulk-equivalent duration.
        const float dur = (float)(now - step_start_us);
        if (first_step_pending) {
            first_step_pending = false;
        } else {
            float be = dur / charge_step_weight(step);
            if (be > kMaxStepUs) be = kMaxStepUs;   // clamp under-load/anomalous outliers
            ring[ring_head] = be;
            ring_head = (ring_head + 1) % kRing;
            if (ring_count < kRing) ring_count++;
        }
        cur_step = step;
        step_start_us = now;
    } else if (step != cur_step) {
        // Multi-step jump (e.g. woke from sleep across several steps) or a
        // small dip under heavy use — can't attribute timing cleanly, so just
        // resync without polluting the ring.
        cur_step = step;
        step_start_us = now;
        first_step_pending = false;
    }

    g_charge_eta.charging = true;
    if (cur_step < 10) {
        // Use the measured rate once we have a timed step; until then fall back
        // to the default rate and flag the estimate provisional (renders "?").
        const bool measured = (ring_count > 0);
        float bulk;
        if (measured) {
            // Median of the timed steps — robust to a single slow/fast outlier
            // in a way the old mean wasn't (one 47-min under-load step used to
            // drag the whole projection up). kRing is tiny, so insertion-sort.
            float tmp[kRing];
            for (int i = 0; i < ring_count; i++) tmp[i] = ring[i];
            for (int i = 1; i < ring_count; i++) {
                const float v = tmp[i];
                int j = i - 1;
                while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
                tmp[j + 1] = v;
            }
            bulk = tmp[ring_count / 2];
        } else {
            bulk = kDefaultStepUs;
        }
        float rem_us = 0.0f;
        for (int L = cur_step + 1; L <= 10; L++) rem_us += bulk * charge_step_weight(L);
        int mins = (int)(rem_us / 60000000.0f + 0.5f);
        if (mins < 0)   mins = 0;
        if (mins > 999) mins = 999;
        g_charge_eta.valid = true;
        g_charge_eta.provisional = !measured;
        g_charge_eta.minutes = mins;
    } else {
        // cur_step == 10 → essentially full; nothing meaningful to count down.
        g_charge_eta.valid = true;
        g_charge_eta.provisional = false;
        g_charge_eta.minutes = 0;
    }
}

__attribute__((noinline)) void render_screen() {
    fb_clear();

    const bool connected = bt_is_connected();

    // Status header: product + numeric firmware version from CMake.
    char title[32];
    snprintf(title, sizeof(title), "DS5Dongle %s", FIRMWARE_VERSION_SHORT);
    draw_text(kContentX, 0, title);
    draw_icon(120, 0, connected ? kIconLinkOn : kIconLinkOff, 8, 8);

    if (connected) {
        uint8_t a[6];
        bt_get_addr(a);
        char buf[24];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 a[0], a[1], a[2], a[3], a[4], a[5]);
        draw_text(kContentX, 9, buf);

        const uint8_t pwr = interrupt_in_data[52];
        int pct = (pwr & 0x0F) * 10;
        if (pct > 100) pct = 100;
        const uint8_t pstate = pwr >> 4;
        char marker = ' ';
        if (pstate == 1) marker = '+';      // Charging
        else if (pstate == 2) marker = '*'; // Complete
        else if (pstate >= 0xA) marker = '!'; // Error
        char bbuf[16];
        snprintf(bbuf, sizeof(bbuf), "%3d%%%c", pct, marker);
        draw_text(kContentX, 18, bbuf);
        draw_battery_icon(36, 18, pct);

        // Charge ETA, right of the battery icon (icon ends at x≈90). Shown only
        // while charging: "~43m?" is the provisional default-rate estimate shown
        // immediately on plug-in; the "?" drops to "~43m" once a real 10% step
        // has been timed and the measured rate takes over. See sample_charge_eta().
        if (g_charge_eta.charging) {
            char ebuf[8];
            if (g_charge_eta.valid)
                snprintf(ebuf, sizeof(ebuf), "~%dm%s", g_charge_eta.minutes,
                         g_charge_eta.provisional ? "?" : "");
            else
                snprintf(ebuf, sizeof(ebuf), "~--m");
            draw_text(94, 18, ebuf);
        }

        // Left-half visuals are shifted right by kContentX so the < button
        // chrome at (x=0, y=49) doesn't paint over the live stick dot.
        rect_outline(kContentX, 30, 32, 32);
        int lx = (kContentX + 2) + (interrupt_in_data[0] * 27) / 255;
        int ly = 32 + (interrupt_in_data[1] * 27) / 255;
        rect_filled(lx - 1, ly - 1, 3, 3);
        // L3 (left stick click) — invert the whole box as a pressed indicator.
        if (interrupt_in_data[8] & 0x40) rect_invert(kContentX, 30, 32, 32);

        rect_outline(96, 30, 32, 32);
        int rx = 98 + (interrupt_in_data[2] * 27) / 255;
        int ry = 32 + (interrupt_in_data[3] * 27) / 255;
        rect_filled(rx - 1, ry - 1, 3, 3);
        // R3 (right stick click) — invert the whole box.
        if (interrupt_in_data[8] & 0x80) rect_invert(96, 30, 32, 32);

        // L2/R2 analog trigger bars (vertical, fill from bottom). L2 sits
        // just right of the shifted left stick box.
        rect_outline(kContentX + 32, 33, 4, 29);
        const int l2_fill = (interrupt_in_data[4] * 27) / 255;
        if (l2_fill > 0) rect_filled(kContentX + 33, 61 - l2_fill, 2, l2_fill);
        rect_outline(92, 33, 4, 29);
        const int r2_fill = (interrupt_in_data[5] * 27) / 255;
        if (r2_fill > 0) rect_filled(93, 61 - r2_fill, 2, r2_fill);

        const uint8_t b7 = interrupt_in_data[7];
        const uint8_t b8 = interrupt_in_data[8];

        // D-pad indicator with real arrows instead of generic squares.
        // Primary and diagonal presses light the relevant arrow boxes.  The
        // whole center cluster is shifted a few pixels left to better balance
        // the freed OLED column and keep the PS label away from Cross.
        const int dp = b7 & 0x0F;
        const bool dp_n = (dp == 7 || dp == 0 || dp == 1);
        const bool dp_e = (dp == 1 || dp == 2 || dp == 3);
        const bool dp_s = (dp == 3 || dp == 4 || dp == 5);
        const bool dp_w = (dp == 5 || dp == 6 || dp == 7);
        auto dpad_icon = [&](int x, int y, char dir, bool on) {
            draw_arrow_icon(x, y, dir);
            draw_status_icon_box(x - 1, y - 1, 9, 9, on);
        };
        const int dcx = 46, dcy = 45;
        dpad_icon(dcx,     dcy - 11, 'U', dp_n);
        dpad_icon(dcx + 9, dcy - 2,  'R', dp_e);
        dpad_icon(dcx,     dcy + 7,  'D', dp_s);
        dpad_icon(dcx - 9, dcy - 2,  'L', dp_w);

        // Face buttons use real PlayStation symbols. Pressed buttons are
        // inverted so they pop clearly.
        auto face_icon = [&](int x, int y, int kind, bool on) {
            if      (kind == 0) draw_tri_icon(x, y + 1);
            else if (kind == 1) draw_circle_icon(x, y);
            else if (kind == 2) draw_cross_icon(x, y);
            else                draw_square_icon(x, y);
            draw_status_icon_box(x - 1, y - 1, 9, 9, on);
        };
        const int fcx = 74, fcy = 45;
        face_icon(fcx,     fcy - 11, 0, b7 & 0x80); // Triangle
        face_icon(fcx + 9, fcy - 2,  1, b7 & 0x40); // Circle
        face_icon(fcx,     fcy + 7,  2, b7 & 0x20); // Cross
        face_icon(fcx - 9, fcy - 2,  3, b7 & 0x10); // Square

        // Shoulder / system buttons. L1/R1 are now shown as equal text labels
        // instead of uneven bars. Mute is intentionally omitted from Status to
        // keep the lower center clean.
        draw_status_text_button(37, 27, "L1", b8 & 0x01);
        draw_create_icon(51, 27);
        draw_status_icon_box(50, 26, 9, 9, b8 & 0x10);
        draw_touchpad_icon(61, 27);
        draw_status_icon_box(60, 26, 15, 9, interrupt_in_data[9] & 0x02);
        draw_options_icon(77, 27);
        draw_status_icon_box(76, 26, 9, 9, b8 & 0x20);
        draw_status_text_button(85, 27, "R1", b8 & 0x02);

        draw_ps_text_icon(58, 56);
        draw_status_icon_box(57, 55, 14, 9, interrupt_in_data[9] & 0x01);

        // L2/R2 analog bars already show travel. Keep them white-on-black even
        // when the digital trigger bit is set; do not invert the trigger bars.
    } else {
        if (ui_hebrew()) {
            draw_hebrew_r(126, 14, "לצימוד שלט חדש");
            draw_text(36, 26, "Create + PS");
            draw_hebrew_r(126, 26, "לחץ");
            draw_hebrew_r(126, 38, "המתן להבהוב כחול");
        } else {
            draw_text(kContentX, 14, "Pair your DualSense:");
            draw_text(kContentX, 26, "1. Hold Create + PS");
            draw_text(kContentX, 36, "2. Wait for light bar");
            draw_text(kContentX, 46, "   to flash blue");
        }
    }

    flush_fb();
}

__attribute__((noinline)) void render_screen_rssi() {
    fb_clear();
    draw_title("BT Signal", "אות בלוטות");
    if (bt_is_connected()) {
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        char buf[24];
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", (int)rssi);
        draw_text(kContentX, 12, buf);  // Keep RSSI line in English by request.

        // Map RSSI range -90..-40 dBm to 0..100% bar
        int pct = ((int)rssi + 90) * 100 / 50;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (ui_hebrew()) {
            snprintf(buf, sizeof(buf), "איכות: %d%%", pct);
            draw_hebrew_r(126, 22, buf);
        } else {
            snprintf(buf, sizeof(buf), "Quality: %d%%", pct);
            draw_text(kContentX, 22, buf);
        }
        rect_outline(kContentX, 34, 122, 10);
        int fill = (pct * 118) / 100;
        if (fill > 0) rect_filled(kContentX + 2, 36, fill, 6);

        const char *label = "Poor";
        const char *label_he = "חלש";
        if (rssi > -55) { label = "Excellent"; label_he = "מצוין"; }
        else if (rssi > -65) { label = "Good"; label_he = "טוב"; }
        else if (rssi > -75) { label = "Fair"; label_he = "סביר"; }
        if (ui_hebrew()) {
            snprintf(buf, sizeof(buf), "קישור: %s", label_he);
            draw_hebrew_r(126, 48, buf);
        } else {
            snprintf(buf, sizeof(buf), "Link: %s", label);
            draw_text(kContentX, 48, buf);
        }
    } else {
        draw_no_controller(kContentX, 30);
    }
    flush_fb();
}

// Diagnostics screen state. Read-only viewport that scrolls with controller
// D-pad up/down. No cursor — there's nothing to select.
int   diag_scroll = 0;
uint8_t diag_last_dpad = 8; // edge-trigger N/E/S/W like settings_handle_input

// Per-second rates sampled once per render, shared across format_diag_row's
// rate-based rows so they stay in sync.
struct DiagRates {
    uint32_t usb_rate;
    uint32_t bt_rate;
    uint32_t mic_rate;
    uint32_t bt31_rate;
};
DiagRates g_diag_rates{};

void sample_diag_rates() {
    static uint32_t prev_us_frames = 0, prev_bt_packets = 0, prev_mic_frames = 0, prev_bt31 = 0;
    static uint32_t prev_sample_us = 0;
    const uint32_t now_us = time_us_32();
    const uint32_t cur_us_frames  = audio_usb_frames();
    const uint32_t cur_bt_packets = audio_bt_packets();
    const uint32_t cur_mic_frames = audio_mic_frames();
    const uint32_t cur_bt31       = bt_31_packet_count();
    if (prev_sample_us != 0 && now_us > prev_sample_us) {
        const uint32_t dt_us = now_us - prev_sample_us;
        if (dt_us > 0) {
            g_diag_rates.usb_rate  = (uint32_t)(((uint64_t)(cur_us_frames  - prev_us_frames)  * 1000000u) / dt_us);
            g_diag_rates.bt_rate   = (uint32_t)(((uint64_t)(cur_bt_packets - prev_bt_packets) * 1000000u) / dt_us);
            g_diag_rates.mic_rate  = (uint32_t)(((uint64_t)(cur_mic_frames - prev_mic_frames) * 1000000u) / dt_us);
            g_diag_rates.bt31_rate = (uint32_t)(((uint64_t)(cur_bt31       - prev_bt31)       * 1000000u) / dt_us);
        }
    }
    prev_us_frames  = cur_us_frames;
    prev_bt_packets = cur_bt_packets;
    prev_mic_frames = cur_mic_frames;
    prev_bt31       = cur_bt31;
    prev_sample_us  = now_us;
}

// Row list ordered by relevance: always-useful at top, parked-mic-investigation
// data at bottom. To add a row, bump kNumDiagRows and add a case.
constexpr int kNumDiagRows = 13;
__attribute__((noinline))
void format_diag_row(int idx, char* line, size_t n) {
    switch (idx) {
        case 0: {
            const uint32_t s = time_us_32() / 1000000u;
            snprintf(line, n, "Up:%luh %02lum %02lus",
                     (unsigned long)(s / 3600u),
                     (unsigned long)((s / 60u) % 60u),
                     (unsigned long)(s % 60u));
            break;
        }
        case 1:
            snprintf(line, n, "BT: %s", bt_is_connected() ? "connected" : "waiting");
            break;
        case 2:
            snprintf(line, n, "host02: %lu", (unsigned long)host_out02_total());
            break;
        case 3:
            snprintf(line, n, "trig %lu / tx %lu",
                     (unsigned long)host_out02_trig_allow(),
                     (unsigned long)host_out02_to_bt());
            break;
        case 4:
            // Trigger reports folded into the 0x36 audio path (speaker active),
            // not sent as 0x31. trig == tx-trig-share + this → no drops (#6).
            snprintf(line, n, "trig fold: %lu", (unsigned long)host_out02_trig_folded());
            break;
        case 5:
            snprintf(line, n, "BT31 in: %lu/s", (unsigned long)g_diag_rates.bt31_rate);
            break;
        case 6:
            snprintf(line, n, "USB aud: %lu/s", (unsigned long)g_diag_rates.usb_rate);
            break;
        case 7:
            snprintf(line, n, "BT32 out: %lu/s", (unsigned long)g_diag_rates.bt_rate);
            break;
        case 8:
            snprintf(line, n, "Mic in: %lu/s", (unsigned long)g_diag_rates.mic_rate);
            break;
        case 9:
            snprintf(line, n, "Mic dec=%ld w=%u",
                     (long)audio_mic_last_decoded(),
                     (unsigned)audio_mic_last_wrote());
            break;
        case 10:
            snprintf(line, n, "Mic PLC: %lu", (unsigned long)audio_mic_plc_frames());
            break;
        case 11:
            snprintf(line, n, "Jack H%d M%d E%d %02X/%02X",
                     audio_headphones_plugged() ? 1 : 0,
                     audio_headset_mic_plugged() ? 1 : 0,
                     audio_external_mic_active() ? 1 : 0,
                     audio_jack_flags53(), audio_jack_flags54());
            break;
        case 12: {
            uint8_t pfx[6]; bt_31_mic_prefix(pfx);
            snprintf(line, n, "%02X %02X %02X %02X %02X %02X",
                     pfx[0], pfx[1], pfx[2], pfx[3], pfx[4], pfx[5]);
            break;
        }
        default:
            line[0] = '\0';
            break;
    }
}

void diag_handle_input(int visible) {
    if (!bt_is_connected()) return;
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    if (controller_screen_nav_combo_active()) { diag_last_dpad = dpad; return; }
    if (dpad != diag_last_dpad && dpad != 8) {
        if      (dpad == 0) diag_scroll--; // up
        else if (dpad == 4) diag_scroll++; // down
    }
    diag_last_dpad = dpad;
    const int max_top = (kNumDiagRows > visible) ? (kNumDiagRows - visible) : 0;
    if (diag_scroll < 0) diag_scroll = 0;
    if (diag_scroll > max_top) diag_scroll = max_top;
}

__attribute__((noinline)) void render_screen_diag() {
    fb_clear();
    draw_title("Diagnostics", "אבחון");

    sample_diag_rates();
    constexpr int kVisible = 5;
    diag_handle_input(kVisible);

    char line[28];
    for (int i = 0; i < kVisible && diag_scroll + i < kNumDiagRows; i++) {
        format_diag_row(diag_scroll + i, line, sizeof(line));
        draw_text(kContentX, 9 + i * 9, line);
    }

    // Scroll indicators along the right edge, adjacent to the visible content
    // rather than in a footer — keeps the bottom row available for content.
    if (diag_scroll > 0)                       draw_text(120, 9,  "^");
    if (diag_scroll + kVisible < kNumDiagRows) draw_text(120, 45, "v");

    flush_fb();
}

// △ rising edge on the Trigger Test screen cycles trigger_preset and
// re-applies the new effect to the paired controller. KEY1 used to do
// this; moving it to the controller frees K0/K1 for navigation only.
void triggers_handle_input() {
    if (!bt_is_connected()) { triggers_last_face = 0; return; }
    const uint8_t face = interrupt_in_data[7] & 0xF0;
    const bool tri_now  = (face & 0x80) != 0;
    const bool tri_prev = (triggers_last_face & 0x80) != 0;
    if (tri_now && !tri_prev) {
        trigger_preset = (trigger_preset + 1) % kNumTrigPresets;
        send_trigger_effect(trigger_preset);
    }
    triggers_last_face = face;
}

__attribute__((noinline)) void render_screen_triggers() {
    triggers_handle_input();
    fb_clear();
    draw_title("Trigger Test", "בדיקת טריגר");

    char buf[24];
    if (ui_hebrew()) {
        const char* const he_names[] = {"כבוי", "משוב", "נשק", "רטט", "קשת", "דהירה", "מכונה"};
        snprintf(buf, sizeof(buf), "מצב: %s", he_names[trigger_preset]);
        draw_hebrew_r(126, 12, buf);
    } else {
        snprintf(buf, sizeof(buf), "Mode: %s", kTrigPresetNames[trigger_preset]);
        draw_text(kContentX, 12, buf);
    }

    if (bt_is_connected()) {
        const uint8_t l2 = interrupt_in_data[4];
        const uint8_t r2 = interrupt_in_data[5];
        snprintf(buf, sizeof(buf), "L2:%3d  R2:%3d", l2, r2);
        draw_text(kContentX, 24, buf);

        rect_outline(kContentX, 35, 56, 9);
        int lfill = (l2 * 52) / 255;
        if (lfill > 0) rect_filled(kContentX + 2, 37, lfill, 5);
        rect_outline(72, 35, 56, 9);
        int rfill = (r2 * 52) / 255;
        if (rfill > 0) rect_filled(74, 37, rfill, 5);
    } else {
        draw_no_controller(kContentX, 24);
    }

    if (ui_hebrew()) {
        const char *txt = "מחליף";
        draw_hebrew_r(126, 56, txt);
        // Keep the triangle before the Hebrew action label, with a clear gap.
        // The word is right-aligned, so the icon must be placed left of the
        // rendered word, not on top of its left edge.
        draw_tri_icon(126 - hebrew_text_width(txt) - 12, 57);
    } else {
        draw_tri_footer_en(kContentX, 56, "=cycle");
    }
    flush_fb();
}

// --- IMU calibration (DS5 feature report 0x05) ---------------------------
// The DualSense ships per-unit gyro/accel calibration in feature report 0x05,
// which bt.cpp already fetches and caches at connect (init_feature). Parsing it
// lets the Gyro Tilt screen and the tilt->RGB lightbar mode use bias- and
// sensitivity-corrected accel instead of raw counts, so the tilt dot recenters
// per controller. Parse + apply mirror SDL's SDL_hidapi_ps5.c (zlib-licensed)
// LoadCalibrationData/ApplyCalibrationData (credit); feature_data[0x05]'s byte
// layout matches SDL's data[] (index 0 = report id, calibration words from 1).
//
// imu_apply keeps accel in the same +-8192 == 1g count space the callers already
// scale by, so existing /8192 (gyro screen) and +-8192 (lightbar) math is
// unchanged — calibration only removes the per-axis zero offset and corrects gain.
struct ImuCal { int16_t bias; float sens; };  // 0..2 gyro P/Y/R, 3..5 accel X/Y/Z
ImuCal g_imu_cal[6];
bool   g_imu_cal_valid = false;   // a plausible calibration was loaded
bool   g_imu_cal_tried = false;   // 0x05 has been seen this connection (good or bad)

constexpr float kGyroResPerDeg = 1024.0f;
constexpr float kAccelResPerG  = 8192.0f;

inline int16_t cal_ld16(const std::vector<uint8_t>& d, int i) {
    return (int16_t)((uint16_t)d[i] | ((uint16_t)d[i + 1] << 8));
}

__attribute__((noinline))
void imu_cal_parse(const std::vector<uint8_t>& d) {
    g_imu_cal_valid = false;
    if (d.size() < 35) return;                 // SDL requires >= 35 calibration bytes

    const int16_t gPB = cal_ld16(d, 1),  gYB = cal_ld16(d, 3),  gRB = cal_ld16(d, 5);
    const int16_t gPp = cal_ld16(d, 7),  gPm = cal_ld16(d, 9);
    const int16_t gYp = cal_ld16(d, 11), gYm = cal_ld16(d, 13);
    const int16_t gRp = cal_ld16(d, 15), gRm = cal_ld16(d, 17);
    const int16_t gSp = cal_ld16(d, 19), gSm = cal_ld16(d, 21);
    const int16_t aXp = cal_ld16(d, 23), aXm = cal_ld16(d, 25);
    const int16_t aYp = cal_ld16(d, 27), aYm = cal_ld16(d, 29);
    const int16_t aZp = cal_ld16(d, 31), aZm = cal_ld16(d, 33);

    const float num = (float)(gSp + gSm) * kGyroResPerDeg;
    g_imu_cal[0] = { gPB, num / (float)(gPp - gPm) };
    g_imu_cal[1] = { gYB, num / (float)(gYp - gYm) };
    g_imu_cal[2] = { gRB, num / (float)(gRp - gRm) };

    int16_t r;
    r = aXp - aXm; g_imu_cal[3] = { (int16_t)(aXp - r / 2), 2.0f * kAccelResPerG / (float)r };
    r = aYp - aYm; g_imu_cal[4] = { (int16_t)(aYp - r / 2), 2.0f * kAccelResPerG / (float)r };
    r = aZp - aZm; g_imu_cal[5] = { (int16_t)(aZp - r / 2), 2.0f * kAccelResPerG / (float)r };

    // Sanity gate (same as SDL): a wild bias or a gain off by >50% means a bad
    // factory cal or a short/garbled read — fall back to raw rather than amplify it.
    for (int i = 0; i < 6; i++) {
        const float divisor = (i < 3) ? 64.0f : 1.0f;
        const int   ab      = g_imu_cal[i].bias < 0 ? -g_imu_cal[i].bias : g_imu_cal[i].bias;
        float       gain    = 1.0f - g_imu_cal[i].sens / divisor;
        if (gain < 0) gain = -gain;
        if (ab > 1024 || gain > 0.5f) return;  // leave g_imu_cal_valid = false
    }
    g_imu_cal_valid = true;
}

// Poll once per frame from oled_loop: parse 0x05 the first time it is available
// for this controller, and reset on disconnect so the next controller re-reads.
void imu_cal_service() {
    if (!bt_is_connected()) { g_imu_cal_valid = false; g_imu_cal_tried = false; return; }
    if (g_imu_cal_tried) return;
    auto d = bt_peek_feature(0x05);
    if (d.size() < 35) return;                 // not arrived yet — retry next frame
    imu_cal_parse(d);
    g_imu_cal_tried = true;
}

// index 0..2 gyro, 3..5 accel. Returns the calibrated value in the same count
// scale the raw value used (+-8192 == 1g for accel); identity when no valid
// calibration is loaded, so behaviour matches the pre-calibration firmware.
inline int16_t imu_apply(int index, int16_t raw) {
    if (!g_imu_cal_valid) return raw;
    return (int16_t)((float)(raw - g_imu_cal[index].bias) * g_imu_cal[index].sens);
}

__attribute__((noinline)) void render_screen_gyro() {
    fb_clear();
    draw_title("Gyro Tilt", "גיירו");
    if (bt_is_connected()) {
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        ax = imu_apply(3, ax);  // bias/sensitivity-corrected accel (identity if no cal)
        ay = imu_apply(4, ay);
        az = imu_apply(5, az);
        char buf[16];
        snprintf(buf, sizeof(buf), "X%+5d", ax); draw_text(kContentX, 10, buf);
        snprintf(buf, sizeof(buf), "Y%+5d", ay); draw_text(50, 10, buf);
        snprintf(buf, sizeof(buf), "Z%+5d", az); draw_text(94, 10, buf);

        const int bx = 44, by = 22, bw = 40, bh = 40;
        rect_outline(bx, by, bw, bh);
        for (int x = bx + 1; x < bx + bw - 1; x++) px(x, by + bh / 2, true);
        for (int y = by + 1; y < by + bh - 1; y++) px(bx + bw / 2, y, true);
        // Plot the two axes that read ~0 when the controller lies flat: X (roll,
        // left/right) and Z (pitch, fwd/back). Gravity rests on Y when flat, so
        // driving the dot from Y pegged it to the bottom edge at rest — using Z
        // keeps the dot centred flat and it tracks as you tilt. (Readout above
        // still shows all three raw axes.)
        // Negated so the dot follows the tilt direction: tilt left -> dot left,
        // tilt forward -> dot up (gravity pulls the opposite way on the axis).
        int dx = -((int)ax * (bw / 2 - 3)) / 8192;
        int dy = -((int)az * (bh / 2 - 3)) / 8192;
        int cx = bx + bw / 2 + dx;
        int cy = by + bh / 2 + dy;
        if (cx < bx + 2) cx = bx + 2;
        if (cx > bx + bw - 3) cx = bx + bw - 3;
        if (cy < by + 2) cy = by + 2;
        if (cy > by + bh - 3) cy = by + bh - 3;
        rect_filled(cx - 1, cy - 1, 3, 3);
    } else {
        draw_no_controller(kContentX, 30);
    }
    flush_fb();
}

__attribute__((noinline)) void render_screen_touchpad() {
    fb_clear();
    draw_title("Touchpad", "משטח מגע");
    if (bt_is_connected()) {
        rect_outline(kContentX + 2, 12, 116, 30);
        int active = 0;
        for (int finger = 0; finger < 2; finger++) {
            const int off = 32 + finger * 4;
            const uint32_t f = (uint32_t)interrupt_in_data[off] |
                               ((uint32_t)interrupt_in_data[off + 1] << 8) |
                               ((uint32_t)interrupt_in_data[off + 2] << 16) |
                               ((uint32_t)interrupt_in_data[off + 3] << 24);
            const bool not_touching = (f >> 7) & 1u;
            if (not_touching) continue;
            const uint16_t fx = (f >> 8) & 0xFFFu;
            const uint16_t fy = (f >> 20) & 0xFFFu;
            int sx = (kContentX + 3) + ((int)fx * 110) / 1919;
            int sy = 13 + ((int)fy * 26) / 1079;
            if (sx < kContentX + 3)   sx = kContentX + 3;
            if (sx > 122) sx = 122;
            if (sy < 13)  sy = 13;
            if (sy > 40)  sy = 40;
            rect_filled(sx - 1, sy - 1, 3, 3);
            active++;
        }
        char buf[20];
        if (ui_hebrew()) {
            snprintf(buf, sizeof(buf), "אצבעות: %d", active);
            draw_hebrew_r(126, 46, buf);
        } else {
            snprintf(buf, sizeof(buf), "Fingers: %d", active);
            draw_text(kContentX, 46, buf);
        }
    } else {
        draw_no_controller(kContentX, 30);
    }
    flush_fb();
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[4] = 0x04; // valid_flag1: LIGHTBAR_CONTROL_ENABLE (bit 2)
    pkt[47] = r;   // lightbar_red
    pkt[48] = g;   // lightbar_green
    pkt[49] = b;   // lightbar_blue
    bt_write(pkt, sizeof(pkt));
}

// Tiny 32-step sine LUT (no <cmath>). angle 0..255 → amplitude -127..127.
static const int8_t kSine32[32] = {
    0,   24,   49,   70,   90,  106,  117,  125,  127,  125,  117,  106,   90,   70,   49,   24,
    0,  -24,  -49,  -70,  -90, -106, -117, -125, -127, -125, -117, -106,  -90,  -70,  -49,  -24,
};
int sin_lut(uint8_t a) { return kSine32[(a >> 3) & 0x1F]; }

void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (h >= 360) h %= 360;
    const uint8_t region = (uint8_t)(h / 60);
    const uint16_t remainder = (uint16_t)((h - region * 60u) * 256u / 60u);
    const uint8_t p = (uint8_t)(((uint16_t)v * (255u - s)) >> 8);
    const uint8_t q = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * remainder) >> 8))) >> 8);
    const uint8_t t = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * (255u - remainder)) >> 8))) >> 8);
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

const char* lb_mode_tag(int mode) {
    switch (mode) {
        case kLbModeHost: return "[HOST]";
        case kLbModeBattery: return "[BATT]";
        default: return "[HOST]";
    }
}

static bool oled_lightbar_host_mode_internal() {
    return lb_mode == kLbModeHost;
}

// R1 rising edge on Lightbar cycles lb_mode. Used to be KEY1; that moved
// to back-nav. Triangle on this screen stays as "save current RGB to
// favorite slot 0" (the existing favorite-save UX), so R1 is the next
// free button that doesn't break a mental model.
void lightbar_handle_input() {
    if (!bt_is_connected()) { lb_last_buttons = 0; return; }
    const uint8_t btns   = interrupt_in_data[8];
    const bool r1_now    = (btns & 0x02) != 0;
    const bool r1_prev   = (lb_last_buttons & 0x02) != 0;
    if (r1_now && !r1_prev) {
        lb_mode = (lb_mode == kLbModeHost) ? kLbModeBattery : kLbModeHost;
        lb_dirty = true; // persisted on leaving the Lightbar screen
    }
    lb_last_buttons = btns;
}

__attribute__((noinline)) void render_screen_lightbar() {
    lightbar_handle_input();
    fb_clear();
    if (ui_hebrew()) {
        draw_hebrew_r(126, 0, "תאורה");
        draw_text(kContentX, 0, lb_mode_tag(lb_mode));
    } else {
        draw_title("Lightbar", "תאורה");
        draw_text(86, 0, lb_mode_tag(lb_mode));
    }

    if (bt_is_connected()) {
        // lb_r/lb_g/lb_b are computed every frame by lightbar_service() (which
        // runs ahead of this render in oled_loop), so here we only display them.
        char buf[16];
        snprintf(buf, sizeof(buf), "R:%3u", lb_r); draw_text(kContentX, 12, buf);
        snprintf(buf, sizeof(buf), "G:%3u", lb_g); draw_text(48, 12, buf);
        snprintf(buf, sizeof(buf), "B:%3u", lb_b); draw_text(90, 12, buf);

        const int by = 22, bh = 8;
        rect_outline(kContentX,  by, 38, bh); int rf = (lb_r * 34) / 255; if (rf > 0) rect_filled(kContentX + 2,  by + 2, rf, bh - 4);
        rect_outline(48, by, 38, bh); int gf = (lb_g * 34) / 255; if (gf > 0) rect_filled(50, by + 2, gf, bh - 4);
        rect_outline(90, by, 38, bh); int bf = (lb_b * 34) / 255; if (bf > 0) rect_filled(92, by + 2, bf, bh - 4);

        // fixed65: HOST/BATT only, no favorites/effects UI.
        lb_last_face = interrupt_in_data[7] & 0xF0;

        if (ui_hebrew()) draw_hebrew_r(126, 38, "מצבי תאורה"); else draw_text(kContentX, 38, "Modes: HOST / BATT");
        const char* hint =
            (lb_mode == kLbModeHost) ? "Host controls LED" :
                                       "Battery B->R 25%";
        draw_text(kContentX, 48, hint);
        // No send here: lightbar_service() owns pushing the color to the
        // controller every frame, on this screen and every other.
    } else {
        draw_no_controller(kContentX, 30);
    }
    if (ui_hebrew()) {
        draw_hebrew_r(126, 56, "שינוי מצב עם");
        draw_text(kContentX, 56, "R1");
    } else {
        draw_text(kContentX, 56, "R1=mode");
    }
    flush_fb();
}

// Compute lb_r/lb_g/lb_b for an OLED lightbar mode (0..7) plus Battery (9). HOST (8) is handled
// by the caller (no firmware color). noinline keeps the float/HSV literals out
// of lightbar_service's / oled_loop's literal pool (same Thumb reach constraint
// the render_screen_* functions hit).
__attribute__((noinline))
void lightbar_compute_mode(int mode, uint32_t now_ms) {
    if (mode == 0) {
        // LIVE: tilt -> RGB
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        ax = imu_apply(3, ax);  // calibrated accel keeps the +-8192 == 1g scale below
        ay = imu_apply(4, ay);
        az = imu_apply(5, az);
        const int rr = ((int)ax + 8192) * 255 / 16384;
        const int gg = ((int)ay + 8192) * 255 / 16384;
        const int bb = ((int)az + 8192) * 255 / 16384;
        lb_r = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
        lb_g = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
        lb_b = (uint8_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
    } else if (mode <= 4) {
        // FAV slot: fixed color
        const int slot = mode - 1;
        lb_r = lb_fav_r[slot];
        lb_g = lb_fav_g[slot];
        lb_b = lb_fav_b[slot];
    } else if (mode == 5) {
        // BREATHING: modulate FAV0 brightness with a sine wave (~3 s cycle)
        const uint8_t phase = (uint8_t)(now_ms / 12);
        const int s = sin_lut(phase); // -127..127
        const uint16_t scale = (uint16_t)(32 + (s + 127) / 2); // 32..191
        lb_r = (uint8_t)((lb_fav_r[0] * scale) / 255);
        lb_g = (uint8_t)((lb_fav_g[0] * scale) / 255);
        lb_b = (uint8_t)((lb_fav_b[0] * scale) / 255);
    } else if (mode == 6) {
        // RAINBOW: hue sweep over ~6 s
        const uint16_t hue = (uint16_t)((now_ms / 17) % 360);
        hsv_to_rgb(hue, 255, 255, &lb_r, &lb_g, &lb_b);
    } else {
        // FADE between FAV slots, 2 s per slot
        const uint32_t kSlotMs = 2000;
        const uint32_t total = now_ms % (4 * kSlotMs);
        const int slot = (int)(total / kSlotMs);
        const int next = (slot + 1) & 3;
        const uint16_t blend = (uint16_t)(((total - slot * kSlotMs) * 256u) / kSlotMs);
        lb_r = (uint8_t)((lb_fav_r[slot] * (255 - blend) + lb_fav_r[next] * blend) / 255);
        lb_g = (uint8_t)((lb_fav_g[slot] * (255 - blend) + lb_fav_g[next] * blend) / 255);
        lb_b = (uint8_t)((lb_fav_b[slot] * (255 - blend) + lb_fav_b[next] * blend) / 255);
    }
}

// The single owner of the controller LED. Runs every frame (~10 Hz) from
// oled_loop, on every screen, so a chosen mode "sticks" everywhere instead of
// only while the Lightbar screen renders. Priority:
//   1. Charging  -> amber-orange breathing pulse (status indicator).
//   2. lb_mode != HOST -> the selected OLED mode/color.
//   3. HOST (or disconnected) -> hand the LED back to the host/game.
// When the firmware owns the LED it (a) writes state[] so the color rides every
// host/audio packet and (b) actively pushes it via send_lightbar_color so it
// updates even when the host is idle and animations keep moving. g_lightbar_
// override gates state_update() so host AllowLedColor writes can't stomp us.
__attribute__((noinline))
void lightbar_service() {
    if (!bt_is_connected()) { g_lightbar_override = false; return; }
    const uint32_t now_ms = time_us_32() / 1000;

    if (g_charge_eta.charging) {
        // ~4.6 s breathing cycle (256 phase steps × 18 ms). Base amber
        // (255,100,0) sine-enveloped from dim (24) to bright (240).
        const uint8_t  phase = (uint8_t)(now_ms / 18);
        const int      s     = sin_lut(phase);                          // -127..127
        const uint16_t scale = (uint16_t)(24 + ((s + 127) * 216) / 254); // 24..240
        lb_r = (uint8_t)((255u * scale) / 255u);
        lb_g = (uint8_t)((100u * scale) / 255u);
        lb_b = 0;
    } else if (lb_mode == kLbModeHost) {
        // Reflect the host's current LED on the OLED bars, then stand down.
        state_get_led(&lb_r, &lb_g, &lb_b);
        g_lightbar_override = false;
        return;
    } else if (lb_mode == kLbModeBattery) {
        // Ohad fixed27: battery driven touchpad/lightbar color at 25% output.
        // 100% = blue, 0% = red, with a smooth red<->blue gradient.
        int pct = (interrupt_in_data[52] & 0x0F) * 10;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const uint8_t maxv = 64; // 25% of 255
        lb_r = (uint8_t)((100 - pct) * maxv / 100);
        lb_g = 0;
        lb_b = (uint8_t)(pct * maxv / 100);
    } else {
        // Legacy modes are no longer user-facing in fixed65; fall back to HOST.
        lb_mode = kLbModeHost;
        state_get_led(&lb_r, &lb_g, &lb_b);
        g_lightbar_override = false;
        return;
    }

    g_lightbar_override = true;
    state_set_led(lb_r, lb_g, lb_b);  // ride every host/audio frame
    if (!spk_active) {
        // Active push so the LED updates when the host is idle and animations
        // keep moving. Skipped during audio: the 0x36 frames already carry
        // state[]'s LED at audio rate, and slipping a 0x31 between them would
        // intrude on the load-bearing audio/haptic packet cadence.
        send_lightbar_color(lb_r, lb_g, lb_b);
    }
}

void lightbar_load_config() {
    const Config_body& c = get_config();
    lb_mode = c.lightbar_mode;
    if (lb_mode != kLbModeHost && lb_mode != kLbModeBattery) lb_mode = kLbModeHost;
    for (int i = 0; i < 4; i++) {
        lb_fav_r[i] = c.lb_fav_r[i];
        lb_fav_g[i] = c.lb_fav_g[i];
        lb_fav_b[i] = c.lb_fav_b[i];
    }
    lb_dirty = false;
}

void lightbar_save_config() {
    Config_body b = get_config();
    b.lightbar_mode = (uint8_t)lb_mode;
    for (int i = 0; i < 4; i++) {
        b.lb_fav_r[i] = lb_fav_r[i];
        b.lb_fav_g[i] = lb_fav_g[i];
        b.lb_fav_b[i] = lb_fav_b[i];
    }
    set_config(b);
    config_save(); // fixed65am: may defer until USB audio is idle
    lb_dirty = false;
}

__attribute__((noinline)) void render_screen_vu() {
    fb_clear();
    draw_title("Audio Meters", "מד שמע");
    if (bt_is_connected()) {
        const uint8_t spk = audio_peak_speaker();
        const uint8_t hap = audio_peak_haptic();
        char buf[16];
        snprintf(buf, sizeof(buf), "SPK %3u", spk);
        draw_text(kContentX, 14, buf);
        rect_outline(48, 14, 80, 8);
        int sfill = (spk * 76) / 255;
        if (sfill > 0) rect_filled(50, 16, sfill, 4);

        snprintf(buf, sizeof(buf), "HAP %3u", hap);
        draw_text(kContentX, 28, buf);
        rect_outline(48, 28, 80, 8);
        int hfill = (hap * 76) / 255;
        if (hfill > 0) rect_filled(50, 30, hfill, 4);

        if (ui_hebrew()) draw_hebrew_r(126, 42, "שמע חי"); else draw_text(kContentX, 42, "Live USB audio peaks");
    } else {
        draw_no_controller(kContentX, 30);
    }
    flush_fb();
}



// ---- Help screen ---------------------------------------------------------

// Built-in manual-style guide. It uses many short scrollable lines so the OLED
// can replace the external PDF manual for everyday operation.  Each topic starts
// with a small bullet.  The following lines are deliberately sentence-like and
// explanatory instead of a terse shortcut table.
struct HelpLine {
    const char *en;
    const char *he;
    uint8_t flags;
};

constexpr uint8_t kHelpHeader = 0x01;
constexpr uint8_t kHelpHeAscii = 0x02;

const HelpLine kHelpLines[] = {
    {"Built-in guide", "מדריך מובנה", kHelpHeader},
    {"This screen explains", "המסך הזה מסביר", 0},
    {"the menus without PDF.", "את התפריטים במכשיר", 0},
    {"Scroll this help with", "גוללים את העזרה", 0},
    {"D-Pad Up / Down.", "D-Pad Up / Down", kHelpHeAscii},

    {"Screen navigation", "ניווט בין מסכים", kHelpHeader},
    {"Move between OLED", "אפשר לעבור בין", 0},
    {"screens with side keys.", "מסכי המכשיר", 0},
    {"KEY1 goes forward.", "בעזרת כפתורי הצד", 0},
    {"KEY0 goes backward.", "KEY1 הבא", 0},
    {"From the controller:", "KEY0 הקודם", 0},
    {"hold Options and use", "או דרך השלט:", 0},
    {"D-Pad Left / Right.", "Options + DPad L/R", kHelpHeAscii},
    {"Hold KEY0+KEY1", "החזקת שני כפתורי", 0},
    {"to reboot the Pico.", "הצד מפעילה מחדש", 0},

    {"Pairing a controller", "צימוד שלט", kHelpHeader},
    {"If no pad is paired,", "כשאין שלט מחובר", 0},
    {"Status shows pairing", "מסך סטטוס מציג", 0},
    {"instructions on screen.", "הוראות צימוד", 0},
    {"Hold Create + PS", "Create + PS", kHelpHeAscii},
    {"until the light bar", "מחזיקים עד שפס", 0},
    {"flashes blue.", "התאורה מהבהב כחול", 0},
    {"Then wait for BT", "אחר כך ממתינים", 0},
    {"connection to finish.", "לחיבור בלוטות", 0},

    {"Status screen", "מסך סטטוס", kHelpHeader},
    {"Status is the live", "סטטוס הוא המסך", 0},
    {"controller monitor.", "הראשי למצב השלט", 0},
    {"It shows version,", "הוא מציג גרסה", 0},
    {"BT address, battery", "כתובת בלוטות", 0},
    {"and charge state.", "סוללה וטעינה", 0},
    {"The two squares are", "שני הריבועים הם", 0},
    {"left and right sticks.", "הסטיקים ימין ושמאל", 0},
    {"The dot is stick", "הנקודה מראה את", 0},
    {"position in real time.", "מיקום הסטיק בזמן אמת", 0},
    {"L3/R3 invert their", "לחיצת L3 או R3", 0},
    {"stick box when pressed.", "הופכת את ריבוע הסטיק", 0},
    {"L2/R2 are vertical", "L2 ו R2 מוצגים", 0},
    {"analog trigger bars.", "כפסים אנלוגיים", 0},
    {"D-Pad is shown with", "החצים מוצגים", 0},
    {"real arrow icons.", "בסמלי חצים", 0},
    {"Triangle, Circle, X", "משולש עיגול X", 0},
    {"and Square show the", "וריבוע מציגים את", 0},
    {"face buttons.", "כפתורי הפעולה", 0},
    {"L1/R1 are text labels.", "L1 R1 הם כיתוב", 0},
    {"Create, touchpad,", "גם Create משטח מגע", 0},
    {"Options and PS also", "Options ו PS", 0},
    {"have live indicators.", "מקבלים חיווי לחיצה", 0},

    {"Slots screen", "מסך סלוטים", kHelpHeader},
    {"Slots store pairing", "סלוטים שומרים", 0},
    {"records for pads.", "צימודים לשלטים", 0},
    {"There are four slots.", "יש ארבעה סלוטים", 0},
    {"The star marks the", "כוכבית מסמנת את", 0},
    {"active slot.", "הסלוט הפעיל", 0},
    {"Select a slot with", "בוחרים סלוט עם", 0},
    {"D-Pad Up / Down.", "D-Pad Up / Down", kHelpHeAscii},
    {"Press Triangle to", "משולש מעביר את", 0},
    {"switch active slot.", "הסלוט הפעיל", 0},
    {"Hold Square to", "החזקה על ריבוע", 0},
    {"delete one slot.", "מוחקת סלוט אחד", 0},
    {"Empty means no saved", "ריק אומר שאין", 0},
    {"controller in it.", "שלט שמור בסלוט", 0},

    {"Lightbar screen", "מסך תאורה", kHelpHeader},
    {"Lightbar controls", "מסך תאורה שולט", 0},
    {"the controller LED.", "בפס התאורה של השלט", 0},
    {"HOST lets the PC", "HOST נותן למחשב", 0},
    {"choose the color.", "לבחור את הצבע", 0},
    {"BATT shows battery", "BATT מציג צבע", 0},
    {"color from dongle.", "לפי מצב הסוללה", 0},
    {"Use R1 to change", "עם R1 מחליפים", 0},
    {"lightbar mode.", "מצב תאורה", 0},

    {"Triggers screen", "מסך טריגרים", kHelpHeader},
    {"Triggers page is for", "מסך טריגרים מיועד", 0},
    {"testing L2 and R2.", "לבדיקת L2 ו R2", 0},
    {"It helps verify", "הוא עוזר לבדוק", 0},
    {"adaptive feedback.", "משוב אדפטיבי", 0},
    {"Status bars stay", "בסטטוס הפסים", 0},
    {"white-on-black.", "נשארים לבן על שחור", 0},

    {"Gyro screen", "מסך גיירו", kHelpHeader},
    {"Gyro shows motion", "גיירו מציג נתוני", 0},
    {"and tilt values.", "תנועה והטיה", 0},
    {"Use it to confirm", "משתמשים בו כדי", 0},
    {"motion input works.", "לוודא שחיישנים עובדים", 0},

    {"Touchpad screen", "מסך משטח מגע", kHelpHeader},
    {"Touchpad shows", "מסך מגע מציג", 0},
    {"finger positions.", "מיקום אצבעות", 0},
    {"It also shows how", "הוא מציג גם", 0},
    {"many fingers exist.", "כמה אצבעות זוהו", 0},

    {"BT Signal screen", "מסך אות בלוטות", kHelpHeader},
    {"BT Signal shows", "אות בלוטות מציג", 0},
    {"RSSI in dBm.", "RSSI ב dBm", 0},
    {"Closer to zero is", "מספר קרוב לאפס", 0},
    {"usually stronger.", "בדרך כלל חזק יותר", 0},

    {"VU screen", "מסך עוצמת שמע", kHelpHeader},
    {"VU meters show", "מסך VU מציג", 0},
    {"live audio level.", "עוצמת שמע חיה", 0},
    {"Spk is speaker", "Spk הוא רמקול", 0},
    {"audio from host.", "שמע מהמחשב", 0},
    {"Hap is haptic", "Hap הוא ערוץ", 0},
    {"audio energy.", "אנרגיית רטט", 0},

    {"Remap screen", "מסך מיפוי", kHelpHeader},
    {"Remap changes what", "מיפוי משנה מה", 0},
    {"button USB reports.", "הכפתור מדווח למחשב", 0},
    {"First row turns", "השורה הראשונה", 0},
    {"Remap ON or OFF.", "מפעילה או מכבה", 0},
    {"Choose source with", "בוחרים מקור עם", 0},
    {"D-Pad Up / Down.", "D-Pad Up / Down", kHelpHeAscii},
    {"Choose target with", "בוחרים יעד עם", 0},
    {"D-Pad Left / Right.", "D-Pad Left / Right", kHelpHeAscii},
    {"OFF disables a", "OFF מכבה כפתור", 0},
    {"button output.", "בדיווח למחשב", 0},
    {"PicoMic maps a", "PicoMic קושר", 0},
    {"button to local mic.", "כפתור למיקרופון מקומי", 0},
    {"Press Triangle to", "משולש שומר", 0},
    {"save remap changes.", "שינויי מיפוי", 0},

    {"Settings screen", "מסך הגדרות", kHelpHeader},
    {"Settings changes", "הגדרות משנות", 0},
    {"dongle behavior.", "את התנהגות הדונגל", 0},
    {"Move through rows", "עוברים בין שורות", 0},
    {"with D-Pad Up/Down.", "עם D-Pad Up/Down", 0},
    {"Change value with", "משנים ערך עם", 0},
    {"D-Pad Left/Right.", "D-Pad Left/Right", kHelpHeAscii},
    {"Press Triangle to", "משולש שומר", 0},
    {"save settings.", "את ההגדרות", 0},
    {"Danger actions are", "פעולות מסוכנות", 0},
    {"at the bottom.", "נמצאות בסוף", 0},
    {"They require holding", "הן דורשות החזקה", 0},
    {"Triangle on purpose.", "על משולש בכוונה", 0},

    {"Language", "שפה", kHelpHeader},
    {"Language selects", "שפה מחליפה", 0},
    {"English or Hebrew UI.", "בין עברית לאנגלית", 0},

    {"Ctrl", "Ctrl", kHelpHeader | kHelpHeAscii},
    {"Ctrl selects controller", "Ctrl בוחר סוג שלט", 0},
    {"type: DS5, DSE, Auto.", "DS5 / DSE / Auto", kHelpHeAscii},
    {"Auto is safest unless", "Auto מתאים ברוב", 0},
    {"you need to force one.", "המקרים", 0},

    {"Poll", "Poll", kHelpHeader | kHelpHeAscii},
    {"Poll changes USB", "Poll משנה קצב", 0},
    {"report timing.", "דיווח USB", 0},
    {"Use RT for realtime", "RT הוא מצב", 0},
    {"when supported.", "זמן אמת כשנתמך", 0},

    {"Idle", "Idle", kHelpHeader | kHelpHeAscii},
    {"Idle powers off pad", "Idle מכבה שלט", 0},
    {"after no activity.", "אחרי חוסר פעילות", 0},
    {"Off disables this.", "Off מבטל את זה", 0},

    {"AudioKeep", "AudioKeep", kHelpHeader | kHelpHeAscii},
    {"AudioKeep prevents", "AudioKeep מונע", 0},
    {"Idle power-off while", "כיבוי Idle בזמן", 0},
    {"USB audio is active.", "שיש שמע USB פעיל", 0},

    {"Speaker and mic", "שמע ומיקרופון", kHelpHeader},
    {"Spk Vol changes", "Spk Vol משנה", 0},
    {"controller speaker level.", "עוצמת רמקול", 0},
    {"Mic Gain changes", "Mic Gain משנה", 0},
    {"BT mic gain.", "הגבר מיקרופון", 0},
    {"BT Mic enables", "BT Mic מפעיל", 0},
    {"controller mic path.", "נתיב מיקרופון שלט", 0},

    {"Haptics", "רטט והפטיקס", kHelpHeader},
    {"Hap Gain changes", "Hap Gain משנה", 0},
    {"haptic strength.", "עוצמת רטט", 0},
    {"AutoHap creates", "AutoHap יוצר", 0},
    {"haptics from audio.", "רטט מתוך שמע", 0},
    {"AH Gain changes", "AH Gain משנה", 0},
    {"AutoHap strength.", "עוצמת AutoHap", 0},
    {"AH LP changes the", "AH LP משנה", 0},
    {"low-pass filter.", "פילטר נמוכים", 0},

    {"OLED options", "אפשרויות OLED", kHelpHeader},
    {"OLED Bright changes", "OLED Bright משנה", 0},
    {"screen brightness.", "בהירות מסך", 0},
    {"OLED Rot flips the", "OLED Rot הופך", 0},
    {"display 180 degrees.", "את המסך 180 מעלות", 0},
    {"ScrDim sets time", "ScrDim קובע זמן", 0},
    {"until dimming.", "עד עמעום", 0},
    {"ScrOff sets time", "ScrOff קובע זמן", 0},
    {"until screen off.", "עד כיבוי מסך", 0},
    {"CtrlWake lets pad", "CtrlWake נותן", 0},
    {"input wake OLED.", "לשלט להעיר מסך", 0},

    {"Power options", "אפשרויות כוח", kHelpHeader},
    {"PowerCombo enables", "PowerCombo מאפשר", 0},
    {"PS + Options off.", "כיבוי עם PS+Options", 0},
    {"Power-off path", "בכיבוי בטוח", 0},
    {"saves pending data", "הדונגל שומר מידע", 0},
    {"before disconnecting.", "לפני ניתוק השלט", 0},

    {"Reset and wipe", "איפוס ומחיקה", kHelpHeader},
    {"Reset to defaults", "Reset to defaults", kHelpHeAscii},
    {"restores settings.", "מחזיר הגדרות מקור", 0},
    {"Wipe all slots", "Wipe all slots", kHelpHeAscii},
    {"deletes all pairings.", "מוחק את כל הצימודים", 0},
    {"Both require holding", "שניהם דורשים החזקת", 0},
    {"Triangle, not a tap.", "משולש ולא לחיצה קצרה", 0},
};

int help_line_count() {
    return (int)(sizeof(kHelpLines) / sizeof(kHelpLines[0]));
}

int help_max_scroll() {
    constexpr int kVisible = 5;
    const int count = help_line_count();
    return (count > kVisible) ? (count - kVisible) : 0;
}

void help_clamp_scroll() {
    const int max_scroll = help_max_scroll();
    if (help_scroll < 0) help_scroll = 0;
    if (help_scroll > max_scroll) help_scroll = max_scroll;
}

void help_wrap_scroll() {
    const int max_scroll = help_max_scroll();
    if (max_scroll <= 0) { help_scroll = 0; return; }
    if (help_scroll < 0) help_scroll = max_scroll;
    else if (help_scroll > max_scroll) help_scroll = 0;
}

void help_scroll_step(int delta) {
    help_scroll += delta;
    help_wrap_scroll();
    last_activity_us = time_us_64();
    last_render_us = 0;
}

void help_reset_repeat() {
    help_repeat_dpad = 8;
    help_next_repeat_us = 0;
}

void help_handle_input() {
    if (!bt_is_connected()) {
        help_last_dpad = 8;
        help_reset_repeat();
        return;
    }

    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    if (controller_screen_nav_combo_active()) {
        help_last_dpad = dpad;
        help_reset_repeat();
        return;
    }

    const bool scroll_dpad = (dpad == 0 || dpad == 4); // Up / Down
    const uint32_t now = (uint32_t)time_us_32();

    if (dpad != help_last_dpad) {
        if (scroll_dpad) {
            help_scroll_step(dpad == 4 ? +1 : -1);
            help_repeat_dpad = dpad;
            help_next_repeat_us = now + kHelpRepeatStartUs;
        } else {
            help_reset_repeat();
        }
        help_last_dpad = dpad;
        return;
    }

    if (scroll_dpad && dpad == help_repeat_dpad
        && (int32_t)(now - help_next_repeat_us) >= 0) {
        help_scroll_step(dpad == 4 ? +1 : -1);
        help_next_repeat_us = now + kHelpRepeatEveryUs;
    }

    if (!scroll_dpad) help_reset_repeat();
    help_last_dpad = dpad;
}

bool help_hebrew_line_is_ascii(const HelpLine &line) {
    if (line.flags & kHelpHeAscii) return true;
    const unsigned char *p = (const unsigned char *)line.he;
    while (*p) {
        if (*p >= 0x80) return false;
        ++p;
    }
    return true;
}

void draw_help_line_en(const HelpLine &line, int y) {
    int x = kContentX;
    if (line.flags & kHelpHeader) {
        draw_bullet_dot(kContentX, y);
        x += 6;
    }
    draw_text(x, y, line.en);
}

void draw_help_line_he(const HelpLine &line, int y) {
    if (line.flags & kHelpHeader) {
        draw_bullet_dot(123, y);
        if (help_hebrew_line_is_ascii(line)) draw_text(kContentX, y, line.he);
        else                                draw_hebrew_r(118, y, line.he);
        return;
    }

    if (help_hebrew_line_is_ascii(line)) draw_text(kContentX, y, line.he);
    else                                draw_hebrew_r(126, y, line.he);
}

__attribute__((noinline)) void render_screen_help() {
    help_handle_input();
    help_clamp_scroll();

    fb_clear();

    constexpr int kVisible = 5;
    const int count = help_line_count();
    const int page = help_scroll + 1;
    const int pages = (count > kVisible) ? (count - kVisible + 1) : 1;
    char pg[10];
    snprintf(pg, sizeof(pg), "%d/%d", page, pages);

    if (ui_hebrew()) {
        draw_title("Help", "עזרה");
        draw_text(kContentX, 0, pg);
    } else {
        draw_title("Help", "עזרה");
        const int pg_x = 128 - (int)strlen(pg) * 6;
        draw_text(pg_x > 0 ? pg_x : 0, 0, pg);
    }

    for (int i = 0; i < kVisible; ++i) {
        const int idx = help_scroll + i;
        if (idx >= count) break;
        const int y = 10 + i * 10;
        if (ui_hebrew()) draw_help_line_he(kHelpLines[idx], y);
        else             draw_help_line_en(kHelpLines[idx], y);
    }

    flush_fb();
}

// ---- Remap screen --------------------------------------------------------
// Per-button outgoing-HID remap editor. All edits are applied only to the
// report sent to the USB host; raw controller input used by OLED/power-combo
// logic remains untouched.
const char* const kRemapNames[kRemapCount] = {
    "L2", "L1", "Share", "Up", "Left", "Down", "Right", "L3",
    "R2", "R1", "Opt", "Tri", "Circle", "Cross", "Square", "R3",
    "PS", "Pad", "Mute"
};

void remap_screen_load() {
    remap_config_local = get_config();
    remap_get(remap_local);
    remap_dirty = false;
    remap_set_save_status("");
    remap_init_done = true;
}

void remap_set_identity_local() {
    for (int i = 0; i < kRemapCount; i++) remap_local[i] = (uint8_t)i;
    remap_local[kRemapCount - 1] = kRemapTargetPicoMic; // Mute -> PicoMic default
    remap_dirty = true;
}

static int remap_target_to_cycle(uint8_t t) {
    if (t == kRemapTargetOff) return -1;
    if (t == kRemapTargetPicoMic) return kRemapCount;
    if (t < kRemapCount) return (int)t;
    return -1;
}

static uint8_t remap_cycle_to_target(int v) {
    if (v < 0) return kRemapTargetOff;
    if (v == kRemapCount) return kRemapTargetPicoMic;
    return (uint8_t)v;
}

void remap_adjust_target(int src_idx, int delta) {
    // Cycle target through: OFF, L2..Mute, PicoMic.
    int v = remap_target_to_cycle(remap_local[src_idx]);
    v += delta;
    if (v < -1) v = kRemapCount;
    if (v > kRemapCount) v = -1;
    remap_local[src_idx] = remap_cycle_to_target(v);
    remap_dirty = true;
}

bool remap_save_all() {
    set_config(remap_config_local);
    const bool cfg_ok = config_save();
    const bool map_ok = remap_set(remap_local);
    if (cfg_ok && map_ok) {
        remap_dirty = false;
        return true;
    }
    return false;
}

void remap_handle_input() {
    if (!bt_is_connected()) {
        remap_last_dpad = 8;
        remap_reset_repeat();
        return;
    }
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    if (controller_screen_nav_combo_active()) {
        remap_last_dpad = dpad;
        remap_reset_repeat();
        return;
    }
    const uint8_t face = (uint8_t)(interrupt_in_data[7] & 0xF0);
    const uint32_t now = (uint32_t)time_us_32();

    if (dpad != remap_last_dpad) {
        if (dpad_is_up_down(dpad)) {
            remap_scroll_step(dpad);
            remap_repeat_dpad = dpad;
            remap_next_repeat_us = now + kMenuRepeatStartUs;
        } else if (dpad == 6 || dpad == 2) {
            const int delta = (dpad == 2) ? +1 : -1;
            if (remap_sel == kRemapEnableIdx) {
                remap_config_local.remap_enable ^= 1;
                remap_dirty = true;
            } else if (remap_sel >= kRemapFirstButtonIdx && remap_sel < kRemapResetIdx) {
                remap_adjust_target(remap_sel - kRemapFirstButtonIdx, delta);
            } else if (remap_sel == kRemapResetIdx) {
                remap_set_identity_local();
            }
            remap_reset_repeat();
        } else {
            remap_reset_repeat();
        }
        remap_last_dpad = dpad;
    }

    if (dpad_is_up_down(dpad) && dpad == remap_repeat_dpad
        && (int32_t)(now - remap_next_repeat_us) >= 0) {
        remap_scroll_step(dpad);
        remap_next_repeat_us = now + kMenuRepeatEveryUs;
    }
    if (!dpad_is_up_down(dpad)) remap_reset_repeat();

    // Triangle = save. On the Reset item, Triangle first resets to identity,
    // then saves it immediately.
    const bool tri_now = (face & 0x80) != 0;
    const bool tri_prev = (remap_last_face & 0x80) != 0;
    if (!tri_now && tri_prev) {
        if (remap_sel == kRemapResetIdx) remap_set_identity_local();
        remap_set_save_status(remap_save_all() ? "Saved!" : "Save FAIL");
    }
    remap_last_face = face;
}

void format_remap_item(int idx, char* line, size_t n) {
    const char *cur = (idx == remap_sel) ? ">" : " ";
    if (idx == kRemapEnableIdx) {
        snprintf(line, n, "%s Remap %s", cur, remap_config_local.remap_enable ? "on" : "off");
        return;
    }
    if (idx == kRemapResetIdx) {
        snprintf(line, n, "%s Reset defaults", cur);
        return;
    }
    const int src = idx - kRemapFirstButtonIdx;
    const uint8_t tgt = remap_local[src];
    if (tgt == kRemapTargetOff)      snprintf(line, n, "%s %s=Off", cur, kRemapNames[src]);
    else if (tgt == kRemapTargetPicoMic) snprintf(line, n, "%s %s=PicoMic", cur, kRemapNames[src]);
    else                            snprintf(line, n, "%s %s=%s", cur, kRemapNames[src], kRemapNames[tgt % kRemapCount]);
}

// Render Remap rows directly so D-pad and face buttons appear as symbols
// rather than UP/DOWN/LEFT/RIGHT words.  This applies to both English and
// Hebrew UI modes.
static bool remap_is_face_idx(int idx) {
    return idx == 11 || idx == 12 || idx == 13 || idx == 14; // Triangle/Circle/Cross/Square
}

static bool remap_is_dpad_idx(int idx) {
    return idx == 3 || idx == 4 || idx == 5 || idx == 6; // Up/Left/Down/Right
}

static int remap_label_width_px(int idx) {
    if (remap_is_face_idx(idx) || remap_is_dpad_idx(idx)) return 8;
    return (int)strlen(kRemapNames[idx]) * 6;
}

static void draw_remap_label(int x, int y, int idx) {
    switch (idx) {
        case 3:  draw_arrow_icon(x, y, 'U'); break;
        case 4:  draw_arrow_icon(x, y, 'L'); break;
        case 5:  draw_arrow_icon(x, y, 'D'); break;
        case 6:  draw_arrow_icon(x, y, 'R'); break;
        case 11: draw_tri_icon(x, y + 1); break;
        case 12: draw_circle_icon(x, y); break;
        case 13: draw_cross_icon(x, y); break;
        case 14: draw_square_icon(x, y); break;
        default: draw_text(x, y, kRemapNames[idx]); break;
    }
}

static void draw_remap_item_direct(int idx, int y) {
    const bool sel = (idx == remap_sel);
    draw_char(kContentX, y, sel ? '>' : ' ');

    if (idx == kRemapEnableIdx) {
        if (ui_hebrew()) {
            draw_hebrew_r(126, y, remap_config_local.remap_enable ? "מיפוי פעיל" : "מיפוי כבוי");
        } else {
            draw_text(kContentX + 7, y, remap_config_local.remap_enable ? "Remap ON" : "Remap OFF");
        }
        return;
    }

    if (idx == kRemapResetIdx) {
        if (ui_hebrew()) draw_hebrew_r(126, y, "ברירת מחדל");
        else             draw_text(kContentX + 7, y, "Reset defaults");
        return;
    }

    const int src = idx - kRemapFirstButtonIdx;
    const uint8_t tgt = remap_local[src];
    int x = kContentX + 7;
    draw_remap_label(x, y, src);
    x += remap_label_width_px(src) + 3;
    draw_char(x, y, '=');
    x += 8;

    if (tgt == kRemapTargetOff) {
        draw_text(x, y, "Off");
    } else if (tgt == kRemapTargetPicoMic) {
        draw_text(x, y, "PicoMic");
    } else {
        draw_remap_label(x, y, tgt % kRemapCount);
    }
}

__attribute__((noinline)) void render_screen_remap() {
    service_save_status_timeouts();
    if (!remap_init_done) remap_screen_load();
    remap_handle_input();

    fb_clear();
    char buf[24];
    snprintf(buf, sizeof(buf), "Remap %s", remap_dirty ? "(*)" : "   ");
    if (ui_hebrew()) {
        const char *st = oled_status_he(remap_save_status);
        draw_hebrew_r(126, 0, st[0] ? st : (remap_dirty ? "מיפוי *" : "מיפוי"));
    } else {
        draw_text(kContentX, 0, buf);
        if (remap_save_status[0]) draw_text(86, 0, remap_save_status);
    }

    constexpr int kVisible = 5;
    int top = 0;
    if (remap_sel >= kVisible) top = remap_sel - kVisible + 1;
    for (int i = 0; i < kVisible && top + i < kNumRemapItems; i++) {
        draw_remap_item_direct(top + i, 9 + i * 9);
    }

    if (remap_sel == kRemapResetIdx) {
        if (ui_hebrew()) draw_hebrew_r(126, 56, "ימין/שמאל ברירת מחדל");
        else draw_tri_footer_en(kContentX, 56, "=save");
    } else {
        if (ui_hebrew()) draw_tri_footer_he_save();
        else draw_tri_footer_en(kContentX, 56, "=save");
    }
    flush_fb();
}

void settings_adjust(int delta) {
    Config_body &c = settings_local;
    const int item = settings_selected_item();
    settings_dirty = true;
    switch (item) {
        case 0: { // haptics_gain  [1.0, 2.0] step 0.1
            int v = (int)(c.haptics_gain * 10.0f + 0.5f) + delta;
            if (v < 10) v = 10; if (v > 20) v = 20;
            c.haptics_gain = v / 10.0f;
            break;
        }
        case 1: { // speaker_volume  [-100, 0] step 5
            int v = (int)c.speaker_volume + delta * 5;
            if (v < -100) v = -100; if (v > 0) v = 0;
            c.speaker_volume = (float)v;
            break;
        }
        case 2: { // fixed65u Idle choices: Off / 1 / 2 / 3 / 5 / 10 / 20 / 30 min
            static const uint8_t choices[] = {1, 2, 3, 5, 10, 20, 30};
            constexpr int kOffIdx = 0;
            constexpr int kFirstMinuteIdx = 1;
            constexpr int kNumIdleChoices = 1 + (int)(sizeof(choices) / sizeof(choices[0]));

            int idx = 4; // default to 5 min if an older/unknown value is loaded
            if (c.disable_inactive_disconnect) {
                idx = kOffIdx;
            } else {
                for (int i = 0; i < (int)(sizeof(choices) / sizeof(choices[0])); ++i) {
                    if (c.inactive_time == choices[i]) { idx = kFirstMinuteIdx + i; break; }
                }
            }

            idx += (delta > 0) ? 1 : -1;
            if (idx < 0) idx = kNumIdleChoices - 1;
            if (idx >= kNumIdleChoices) idx = 0;

            if (idx == kOffIdx) {
                c.disable_inactive_disconnect = 1;
            } else {
                c.disable_inactive_disconnect = 0;
                c.inactive_time = choices[idx - kFirstMinuteIdx];
            }
            break;
        }
        case 3: c.disable_pico_led ^= 1; break;
        case 4: { // polling_rate_mode  0..2
            int v = (int)c.polling_rate_mode + delta;
            if (v < 0) v = 2; if (v > 2) v = 0;
            c.polling_rate_mode = (uint8_t)v;
            break;
        }
        case 5: { // audio_buffer_length  [16, 128] step 4
            int v = (int)c.audio_buffer_length + delta * 4;
            if (v < 16) v = 16; if (v > 128) v = 128;
            c.audio_buffer_length = (uint8_t)v;
            break;
        }
        case 6: { // controller_mode  0..2
            int v = (int)c.controller_mode + delta;
            if (v < 0) v = 2; if (v > 2) v = 0;
            c.controller_mode = (uint8_t)v;
            break;
        }
        case 7: { // auto_haptics_enable  0..3
            int v = (int)c.auto_haptics_enable + delta;
            if (v < 0) v = 3; if (v > 3) v = 0;
            c.auto_haptics_enable = (uint8_t)v;
            break;
        }
        case 8: { // auto_haptics_gain  [0, 200] step 10
            int v = (int)c.auto_haptics_gain + delta * 10;
            if (v < 0) v = 0; if (v > 200) v = 200;
            c.auto_haptics_gain = (uint8_t)v;
            break;
        }
        case 9: { // auto_haptics_lowpass  0..3
            int v = (int)c.auto_haptics_lowpass + delta;
            if (v < 0) v = 3; if (v > 3) v = 0;
            c.auto_haptics_lowpass = (uint8_t)v;
            break;
        }
        case 10: { // OLED brightness 10..100%, stored as index 0=100, 9=10
            int pct = brightness_percent_from_idx(c.screen_brightness) + delta * 10;
            if (pct < 10) pct = 10; if (pct > 100) pct = 100;
            c.screen_brightness = (uint8_t)((100 - pct) / 10);
            bright_idx = c.screen_brightness; // live preview; Triangle persists
            break;
        }
        case 11: { // OLED rotation 0=Normal, 1=Flip 180
            c.screen_rotation ^= 1;
            oled_flip180 = (c.screen_rotation != 0); // live preview; Triangle persists
            break;
        }
        case 12: { // Mic gain [-24,+12] dB relative to new 0 reference, step 1 dB
            int v = ((int)c.mic_gain_db_plus24 - 24) + delta;
            if (v < -24) v = -24;
            if (v > 12) v = 12;
            c.mic_gain_db_plus24 = (uint8_t)(v + 24);
            break;
        }
        case 13: { // screen_dim_timeout  [0,250] min, 0 = disabled
            int v = (int)c.screen_dim_timeout + delta;
            if (v < 0) v = 0; if (v > 250) v = 250;
            c.screen_dim_timeout = (uint8_t)v;
            break;
        }
        case 14: { // screen_off_timeout  [0,250] min, 0 = disabled
            int v = (int)c.screen_off_timeout + delta;
            if (v < 0) v = 0; if (v > 250) v = 250;
            c.screen_off_timeout = (uint8_t)v;
            break;
        }
        case 15: c.bt_mic_enable ^= 1; break; // BT mic on/off
        case 16: c.controller_wakes_display ^= 1; break; // controller activity wakes OLED on/off
        case 17: c.power_combo_enable ^= 1; break; // PS+Options power-off combo on/off
        case 18: c.keep_awake_on_audio ^= 1; break; // Audio keeps idle poweroff awake
        case 19: c.ui_language ^= 1; break; // OLED UI language English/Hebrew
        case 22: c.usb_mode ^= 1; break; // USB Mode: normal HID+Audio / MIDI only
    }
}
void settings_handle_input() {
    if (!bt_is_connected()) {
        settings_last_dpad = 8;
        settings_reset_repeat();
        return;
    }
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    if (controller_screen_nav_combo_active()) {
        settings_last_dpad = dpad;
        settings_reset_repeat();
        return;
    }
    const uint8_t face = (uint8_t)(interrupt_in_data[7] & 0xF0);
    const uint32_t now = (uint32_t)time_us_32();

    // Up/Down scrolls through Settings. Holding Up/Down repeats quickly and
    // keeps the existing looping behavior from bottom to top and top to bottom.
    // Left/Right still changes the selected value once per press.
    if (dpad != settings_last_dpad) {
        if (dpad_is_up_down(dpad)) {
            settings_scroll_step(dpad);
            settings_repeat_dpad = dpad;
            settings_next_repeat_us = now + kMenuRepeatStartUs;
        } else if (dpad == 6) {
            settings_adjust(-1);
            settings_reset_repeat();
        } else if (dpad == 2) {
            settings_adjust(+1);
            settings_reset_repeat();
        } else {
            settings_reset_repeat();
        }
        settings_last_dpad = dpad;
    }

    if (dpad_is_up_down(dpad) && dpad == settings_repeat_dpad
        && (int32_t)(now - settings_next_repeat_us) >= 0) {
        settings_scroll_step(dpad);
        settings_next_repeat_us = now + kMenuRepeatEveryUs;
    }
    if (!dpad_is_up_down(dpad)) settings_reset_repeat();

    // Triangle handling — Reset and Wipe-slots items both require a 2 s hold;
    // every other item saves edits on a normal short press.
    const bool tri_now = (face & 0x80) != 0;
    const bool tri_prev = (settings_last_face & 0x80) != 0;
    const int selected_item = settings_selected_item();
    const bool is_hold_item = (selected_item == kSettingsResetIdx
                               || selected_item == kSettingsWipeSlotsIdx);
    if (tri_now && !tri_prev) {
        settings_tri_press_us = (uint32_t)time_us_32();
        settings_reset_triggered = false;
    }
    if (is_hold_item && tri_now && !settings_reset_triggered
        && ((uint32_t)time_us_32() - settings_tri_press_us) >= kResetHoldUs) {
        settings_reset_triggered = true;
        if (selected_item == kSettingsResetIdx) {
            config_default();
            if (config_save()) {
                settings_local = get_config();
                bright_idx = settings_local.screen_brightness;
                oled_flip180 = (settings_local.screen_rotation != 0);
                lightbar_load_config(); // refresh RAM lightbar state (no reboot here)
                settings_dirty = false;
                settings_set_save_status(config_save_pending() ? "Reset pending" : "Reset!");
            } else {
                settings_set_save_status("Reset FAIL");
            }
        } else {
            bt_wipe_all_slots();
            settings_set_save_status("Slots wiped!");
        }
    }
    if (!tri_now && tri_prev) {
        if (!is_hold_item && !settings_reset_triggered) {
            const bool usb_mode_changed = (settings_local.usb_mode != get_config().usb_mode);
            settings_local.bt_connect_guard_100ms = 0; // fixed65ak: BT Guard removed/off
            settings_local.out_burst_guard_100ms = 0;  // fixed65ak: OUT Guard removed/off
            set_config(settings_local);
            bright_idx = get_config().screen_brightness;
            oled_flip180 = (get_config().screen_rotation != 0);
            if (usb_mode_changed) {
                // Drop the current host USB audio/HID session before forcing
                // a flash write and rebooting into the other descriptor set.
                tud_disconnect();
                sleep_ms(300);
            }
            const bool save_ok = usb_mode_changed ? config_save_force_now() : config_save();
            settings_set_save_status(save_ok ? (usb_mode_changed ? "USB Reboot" : (config_save_pending() ? "Save pending" : "Saved!")) : "Save FAIL");
            if (save_ok) {
                settings_dirty = false;
                if (usb_mode_changed) {
                    oled_show_message("USB Reboot", 300);
                    sleep_ms(350);
                    watchdog_reboot(0, 0, 10);
                    while (true) { }
                }
            }
        }
        settings_reset_triggered = false;
    }
    settings_last_face = face;
}

__attribute__((noinline)) void format_settings_item(int row, char* line, size_t n) {
    const Config_body &c = settings_local;
    const int idx = settings_item_for_row(row);
    const char *cur = (row == settings_sel) ? ">" : " ";
    switch (idx) {
        case 0: {
            int g = (int)(c.haptics_gain * 10.0f + 0.5f);
            snprintf(line, n, "%s Hap Gain %d.%dx", cur, g / 10, g % 10);
            break;
        }
        case 1: snprintf(line, n, "%s Spk Vol %ddB", cur, (int)c.speaker_volume); break;
        case 2:
            if (c.disable_inactive_disconnect) snprintf(line, n, "%s Idle off", cur);
            else snprintf(line, n, "%s Idle %umin", cur, c.inactive_time);
            break;
        case 3: snprintf(line, n, "%s Pico LED %s", cur, c.disable_pico_led ? "off" : "on"); break;
        case 4: {
            const char* names[3] = {"250Hz", "500Hz", "RT"};
            snprintf(line, n, "%s Poll %s", cur, names[c.polling_rate_mode % 3]);
            break;
        }
        case 5: snprintf(line, n, "%s AudBuf %u", cur, c.audio_buffer_length); break;
        case 6: {
            const char* names[3] = {"DS5", "DSE", "Auto"};
            snprintf(line, n, "%s Ctrl %s", cur, names[c.controller_mode % 3]);
            break;
        }
        case 7: {
            const char* names[4] = {"Off", "Fallback", "Mix", "Replace"};
            snprintf(line, n, "%s AutoHap %s", cur, names[c.auto_haptics_enable & 3]);
            break;
        }
        case 8: snprintf(line, n, "%s AH Gain %u%%", cur, c.auto_haptics_gain); break;
        case 9: {
            const char* names[4] = {"80Hz", "160Hz", "250Hz", "400Hz"};
            snprintf(line, n, "%s AH LP %s", cur, names[c.auto_haptics_lowpass & 3]);
            break;
        }
        case 10: snprintf(line, n, "%s OLED Bright %d%%", cur, brightness_percent_from_idx(c.screen_brightness)); break;
        case 11: snprintf(line, n, "%s OLED Rot %s", cur, c.screen_rotation ? "180" : "Normal"); break;
        case 12: {
            const int db = (int)c.mic_gain_db_plus24 - 24;
            if (db >= 0) snprintf(line, n, "%s Mic Gain +%ddB", cur, db);
            else         snprintf(line, n, "%s Mic Gain %ddB", cur, db);
            break;
        }
        case 13:
            if (c.screen_dim_timeout == 0) snprintf(line, n, "%s ScrDim off", cur);
            else snprintf(line, n, "%s ScrDim %umin", cur, c.screen_dim_timeout);
            break;
        case 14:
            if (c.screen_off_timeout == 0) snprintf(line, n, "%s ScrOff off", cur);
            else snprintf(line, n, "%s ScrOff %umin", cur, c.screen_off_timeout);
            break;
        case 15: snprintf(line, n, "%s BT Mic %s", cur, c.bt_mic_enable ? "on" : "off"); break;
        case 16: snprintf(line, n, "%s CtrlWake %s", cur, c.controller_wakes_display ? "on" : "off"); break;
        case 17: snprintf(line, n, "%s PowerCombo %s", cur, c.power_combo_enable ? "on" : "off"); break;
        case 18: snprintf(line, n, "%s AudioKeep %s", cur, c.keep_awake_on_audio ? "on" : "off"); break;
        case 19: snprintf(line, n, "%s Language %s", cur, c.ui_language ? "HE" : "EN"); break;
        case 22: snprintf(line, n, "%s USB %s", cur, c.usb_mode ? "MIDI" : "Gamepad"); break;
        case 20: snprintf(line, n, "%s Reset to defaults", cur); break;
        case 21: snprintf(line, n, "%s Wipe all slots", cur); break;
    }
}

const char* settings_label_he(int idx) {
    switch (idx) {
        case 0: return "רטט";
        case 1: return "רמקול";
        case 2: return "כיבוי אוטו";
        case 3: return "נורת פיקו";
        case 4: return "דגימה";
        case 5: return "חוצץ שמע";
        case 6: return "סוג שלט";
        case 7: return "רטט שמע";
        case 8: return "עוצמת רטט";
        case 9: return "סינון רטט";
        case 10: return "בהירות";
        case 11: return "סיבוב מסך";
        case 12: return "עוצמת מיק'";
        case 13: return "עמעום מסך";
        case 14: return "כיבוי מסך";
        case 15: return "מיק בלוטות";
        case 16: return "אודיו מעיר";
        case 17: return "קומבו כיבוי";
        case 18: return "שמירת שמע";
        case 19: return "שפה";
        case 22: return "מצב USB";
        case 20: return "איפוס הגדרות";
        case 21: return "מחיקת חיבורים";
    }
    return "";
}

void format_settings_value(int idx, char* out, size_t n) {
    const Config_body &c = settings_local;
    switch (idx) {
        case 0: {
            int g = (int)(c.haptics_gain * 10.0f + 0.5f);
            snprintf(out, n, "%d.%dx", g / 10, g % 10);
            break;
        }
        case 1: snprintf(out, n, "%ddB", (int)c.speaker_volume); break;
        case 2:
            if (c.disable_inactive_disconnect) snprintf(out, n, "off");
            else snprintf(out, n, "%umin", c.inactive_time);
            break;
        case 3: snprintf(out, n, "%s", c.disable_pico_led ? "off" : "on"); break;
        case 4: {
            const char* names[3] = {"250Hz", "500Hz", "RT"};
            snprintf(out, n, "%s", names[c.polling_rate_mode % 3]);
            break;
        }
        case 5: snprintf(out, n, "%u", c.audio_buffer_length); break;
        case 6: {
            const char* names[3] = {"DS5", "DSE", "Auto"};
            snprintf(out, n, "%s", names[c.controller_mode % 3]);
            break;
        }
        case 7: {
            const char* names[4] = {"Off", "Fallback", "Mix", "Replace"};
            snprintf(out, n, "%s", names[c.auto_haptics_enable & 3]);
            break;
        }
        case 8: snprintf(out, n, "%u%%", c.auto_haptics_gain); break;
        case 9: {
            const char* names[4] = {"80Hz", "160Hz", "250Hz", "400Hz"};
            snprintf(out, n, "%s", names[c.auto_haptics_lowpass & 3]);
            break;
        }
        case 10: snprintf(out, n, "%d%%", brightness_percent_from_idx(c.screen_brightness)); break;
        case 11: snprintf(out, n, "%s", c.screen_rotation ? "180" : "Normal"); break;
        case 12: {
            const int db = (int)c.mic_gain_db_plus24 - 24;
            if (db >= 0) snprintf(out, n, "+%ddB", db);
            else         snprintf(out, n, "%ddB", db);
            break;
        }
        case 13:
            if (c.screen_dim_timeout == 0) snprintf(out, n, "off");
            else snprintf(out, n, "%umin", c.screen_dim_timeout);
            break;
        case 14:
            if (c.screen_off_timeout == 0) snprintf(out, n, "off");
            else snprintf(out, n, "%umin", c.screen_off_timeout);
            break;
        case 15: snprintf(out, n, "%s", c.bt_mic_enable ? "on" : "off"); break;
        case 16: snprintf(out, n, "%s", c.controller_wakes_display ? "on" : "off"); break;
        case 17: snprintf(out, n, "%s", c.power_combo_enable ? "on" : "off"); break;
        case 18: snprintf(out, n, "%s", c.keep_awake_on_audio ? "on" : "off"); break;
        case 19: snprintf(out, n, "%s", c.ui_language ? "HE" : "EN"); break;
        case 22: snprintf(out, n, "%s", c.usb_mode ? "MIDI" : "Gamepad"); break;
        case 20: snprintf(out, n, "hold"); break;
        case 21: snprintf(out, n, "hold"); break;
        default: out[0] = 0; break;
    }
}

void draw_settings_item_he(int row, int y) {
    const int idx = settings_item_for_row(row);
    const bool sel = (row == settings_sel);
    draw_char(kContentX, y, sel ? '>' : ' ');
    char value[18];
    format_settings_value(idx, value, sizeof(value));
    draw_text(kContentX + 7, y, value);
    draw_hebrew_r(126, y, settings_label_he(idx));
}

__attribute__((noinline)) void render_screen_settings() {
    service_save_status_timeouts();
    if (!settings_init_done) {
        settings_local = get_config();
        settings_init_done = true;
    }
    settings_handle_input();

    fb_clear();
    constexpr int kVisible = 5;
    int top = 0;
    if (settings_sel >= kVisible) top = settings_sel - kVisible + 1;

    const int selected_item = settings_selected_item();
    if (ui_hebrew()) {
        const char *st = oled_status_he(settings_save_status);
        draw_hebrew_r(126, 0, st[0] ? st : (settings_dirty ? "הגדרות *" : "הגדרות"));
        for (int i = 0; i < kVisible && top + i < kNumSettingsItems; i++) {
            draw_settings_item_he(top + i, 9 + i * 9);
        }
        if (selected_item == kSettingsResetIdx) {
            draw_tri_footer_he_hold("לאיפוס");
        } else if (selected_item == kSettingsWipeSlotsIdx) {
            draw_tri_footer_he_hold("למחיקה");
        } else {
            draw_tri_footer_he_save();
        }
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "Settings %s", settings_dirty ? "(*)" : "   ");
        draw_text(kContentX, 0, buf);
        if (settings_save_status[0]) {
            draw_text(86, 0, settings_save_status);
        }

        char line[28];
        for (int i = 0; i < kVisible && top + i < kNumSettingsItems; i++) {
            format_settings_item(top + i, line, sizeof(line));
            draw_text(kContentX, 9 + i * 9, line);
        }

        if (selected_item == kSettingsResetIdx) {
            draw_tri_footer_en(kContentX, 56, " hold=RESET");
        } else if (selected_item == kSettingsWipeSlotsIdx) {
            draw_tri_footer_en(kContentX, 56, " hold=WIPE");
        } else {
            draw_tri_footer_en(kContentX, 56, "=save");
        }
    }
    flush_fb();
}

// ---- Slots screen (Phase G) ----------------------------------------------
// Multi-slot persistent pairing UI. Modeled on zurce/DS5Dongle-OLED.
// Credit to zurce.

int slots_cursor = -1;             // initialized to active slot on first entry
uint8_t slots_last_dpad = 8;
uint8_t slots_last_face = 0;
uint32_t slots_sq_press_us = 0;
bool slots_wipe_triggered = false;
const char* slots_status = "";
uint32_t slots_status_until_us = 0;
constexpr uint32_t kSlotsWipeHoldUs = 1500000;  // 1.5 s

void slots_handle_input() {
    if (slots_cursor < 0) slots_cursor = bt_get_slot();
    if (bt_is_connected() && controller_screen_nav_combo_active()) {
        slots_last_dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
        return;
    }
    if (!bt_is_connected()) {
        // Even without a DS5 connected we still want to navigate / wipe;
        // we just can't read the controller's D-pad / face inputs. Return
        // here and require KEY0/KEY1 for screen switching.
        slots_last_dpad = 8;
        slots_last_face = 0;
        return;
    }
    const uint8_t dpad = (uint8_t)(interrupt_in_data[7] & 0x0F);
    const uint8_t face = (uint8_t)(interrupt_in_data[7] & 0xF0);

    if (dpad != slots_last_dpad && dpad != 8) {
        if      (dpad == 0) slots_cursor = (slots_cursor - 1 + kNumSlots) % kNumSlots;
        else if (dpad == 4) slots_cursor = (slots_cursor + 1) % kNumSlots;
    }
    slots_last_dpad = dpad;

    // Triangle rising edge: switch to cursor slot if different from active
    const bool tri_now = (face & 0x80) != 0;
    const bool tri_prev = (slots_last_face & 0x80) != 0;
    if (tri_now && !tri_prev) {
        if (slots_cursor != bt_get_slot()) {
            bt_set_slot(slots_cursor);
            slots_status = "Switched!";
            slots_status_until_us = (uint32_t)time_us_32() + 1500000;
        }
    }

    // Square hold 1.5 s: wipe cursor slot
    const bool sq_now = (face & 0x10) != 0;
    const bool sq_prev = (slots_last_face & 0x10) != 0;
    if (sq_now && !sq_prev) {
        slots_sq_press_us = (uint32_t)time_us_32();
        slots_wipe_triggered = false;
    }
    if (sq_now && !slots_wipe_triggered
        && ((uint32_t)time_us_32() - slots_sq_press_us) >= kSlotsWipeHoldUs) {
        slots_wipe_triggered = true;
        bt_forget_slot(slots_cursor);
        slots_status = "Wiped!";
        slots_status_until_us = (uint32_t)time_us_32() + 1500000;
    }
    if (!sq_now && sq_prev) slots_wipe_triggered = false;

    slots_last_face = face;
}

__attribute__((noinline)) void render_screen_slots() {
    slots_handle_input();
    if (slots_cursor < 0) slots_cursor = bt_get_slot();

    fb_clear();
    char hdr[24];
    const int active = bt_get_slot();
    const bool conn = bt_is_connected();
    snprintf(hdr, sizeof(hdr), "Slots         [s%d %s]", active, conn ? "ON" : "--");
    if (ui_hebrew()) draw_hebrew_r(126, 0, "חיבורים");
    else draw_text(kContentX, 0, hdr);

    if (slots_status[0] && (uint32_t)time_us_32() < slots_status_until_us) {
        draw_text(80, 0, slots_status);
    }

    for (int i = 0; i < kNumSlots; i++) {
        char line[28];
        const char *cursor_mark = (i == slots_cursor) ? ">" : " ";
        const char *active_mark = (i == active) ? "*" : " ";
        if (slot_occupied(i)) {
            uint8_t a[6];
            slot_get_addr(i, a);
            snprintf(line, sizeof(line), "%s%d%s %02X:%02X:%02X:%02X:%02X:%02X",
                     cursor_mark, i, active_mark, a[0], a[1], a[2], a[3], a[4], a[5]);
        } else {
            snprintf(line, sizeof(line), "%s%d%s (empty)", cursor_mark, i, active_mark);
        }
        draw_text(kContentX, 9 + i * 9, line);
    }

    if (ui_hebrew()) {
        draw_hebrew_r(126, 56, "החלף");
        draw_tri_icon(88, 57);
        draw_hebrew_r(78, 56, "מחק");
        draw_square_icon(44, 57);
    } else {
        draw_tri_icon(kContentX, 57);
        draw_text(kContentX + 10, 56, "=switch");
        draw_square_icon(68, 57);
        draw_text(78, 56, " hold=wipe");
    }
    flush_fb();
}

bool oled_save_pending_changes_for_poweroff_internal() {
    bool ok = true;
    bool wrote = false;

    // Settings screen edits live in settings_local until Triangle is pressed.
    // A PS+Options power-off could happen before that save gesture, so commit
    // them here once, right before disconnecting the controller.
    if (settings_init_done && settings_dirty) {
        settings_local.bt_connect_guard_100ms = 0; // fixed65ak: BT Guard removed/off
        settings_local.out_burst_guard_100ms = 0;  // fixed65ak: OUT Guard removed/off
        const bool usb_mode_changed = (settings_local.usb_mode != get_config().usb_mode);
        set_config(settings_local);
        bright_idx = get_config().screen_brightness;
        oled_flip180 = (get_config().screen_rotation != 0);
        watchdog_update();
        if (usb_mode_changed ? config_save_force_now() : config_save()) {
            settings_dirty = false;
            settings_set_save_status("Saved!");
        } else {
            ok = false;
            settings_set_save_status("Save FAIL");
        }
        watchdog_update();
        wrote = true;
    }

    // Remap edits are also staged locally until Triangle. Preserve them on
    // power-off instead of silently losing the table after a reconnect/reboot.
    if (remap_init_done && remap_dirty) {
        watchdog_update();
        if (remap_save_all()) {
            remap_set_save_status("Saved!");
        } else {
            ok = false;
            remap_set_save_status("Save FAIL");
        }
        watchdog_update();
        wrote = true;
    }

    // Lightbar mode/favorites normally auto-save when leaving the Lightbar
    // screen. If the user powers off while still on that screen, save now.
    if (lb_dirty) {
        watchdog_update();
        lightbar_save_config();
        watchdog_update();
        wrote = true;
    }

    // DS5Dongle by Ohad 1.0.5:
    // Also commit an already-deferred Save-pending config. Pressing Triangle
    // while USB audio is active stores the new config in RAM and clears
    // settings_dirty, but intentionally delays flash. If the controller is
    // disconnected before the audio quiet window arrives, that pending write
    // would be lost. During poweroff the audio path is frozen, so it is safe to
    // force the pending flash write here before bt_disconnect().
    if (config_save_pending()) {
        watchdog_update();
        if (config_flush_deferred_save_now()) {
            settings_set_save_status("Saved!");
        } else {
            ok = false;
            settings_set_save_status("Save FAIL");
        }
        watchdog_update();
        wrote = true;
    }

    // Give flash/cache and the OLED popup a tiny quiet window before the BT
    // disconnect command is issued by main.cpp.
    if (wrote) sleep_ms(50);
    return ok;
}

void boot_splash() {
    fb_clear();
    auto cx_for = [](const char* s) {
        int n = 0; while (s[n]) n++;
        return (128 - (n * 6 - 1)) / 2;
    };

    // DS5Dongle by Ohad 1.0.5: final branded boot screen.
    // Keep it simple and monochrome so it matches the real 128x64 SH1107 OLED.
    const char* l1 = "DS5Dongle";
    const char* l2 = "by Ohad";
    char vline[18];
    snprintf(vline, sizeof(vline), "v%s", FIRMWARE_VERSION_SHORT);

    draw_text(cx_for(l1), 14, l1);
    draw_text(cx_for(l2), 29, l2);
    draw_text(cx_for(vline), 44, vline);
    flush_fb();
    sleep_ms(1000);
}

} // namespace

// Public wrapper for state_mgr.cpp. The actual OLED/lightbar state lives in
// the anonymous namespace above, so the wrapper must be defined outside it
// to satisfy the external declaration in oled.h.

void oled_return_to_status_screen() {
    oled_return_to_status_screen_internal();
}

bool oled_lightbar_host_mode() {
    return oled_lightbar_host_mode_internal();
}

bool oled_save_pending_changes_for_poweroff() {
    return oled_save_pending_changes_for_poweroff_internal();
}

bool oled_handle_controller_screen_nav_shortcut() {
    return controller_screen_nav_shortcut_internal();
}

void oled_show_message(const char *msg, uint32_t duration_ms) {
    if (msg == nullptr) return;
    const char *shown = ui_hebrew() ? oled_popup_he(msg) : msg;
    strncpy(oled_popup_msg, shown, sizeof(oled_popup_msg) - 1);
    oled_popup_msg[sizeof(oled_popup_msg) - 1] = '\0';
    oled_popup_until_us = (uint32_t)time_us_32() + duration_ms * 1000u;
    last_activity_us = time_us_64();
}

void oled_init() {
    spi_init(spi1, 10 * 1000 * 1000);
    gpio_set_function(kPinCLK, GPIO_FUNC_SPI);
    gpio_set_function(kPinMOSI, GPIO_FUNC_SPI);

    gpio_init(kPinCS);   gpio_set_dir(kPinCS, GPIO_OUT);  gpio_put(kPinCS, 1);
    gpio_init(kPinDC);   gpio_set_dir(kPinDC, GPIO_OUT);  gpio_put(kPinDC, 0);
    gpio_init(kPinRST);  gpio_set_dir(kPinRST, GPIO_OUT); gpio_put(kPinRST, 1);

    gpio_init(kPinKey0); gpio_set_dir(kPinKey0, GPIO_IN); gpio_pull_up(kPinKey0);
    gpio_init(kPinKey1); gpio_set_dir(kPinKey1, GPIO_IN); gpio_pull_up(kPinKey1);

    hw_reset();
    sh1107_init();

    // Restore OLED UI preferences before the boot splash, so even the splash
    // respects the saved orientation/brightness. config_load() already ran in
    // main(), and config_valid() clamps both fields.
    bright_idx = get_config().screen_brightness;
    oled_flip180 = (get_config().screen_rotation != 0);
    sh1107_set_contrast(kBrightLevels[bright_idx]);

    fb_clear();
    boot_splash();

    // Restore the persisted lightbar mode + favorites. Defaults to HOST
    // passthrough on a fresh flash.
    lightbar_load_config();
}

// Dim-tier renderer: blank the panel and draw a tiny "I'm alive" dot that
// breathes (1s on / 1s off) and walks through 8 evenly-spaced positions every
// ~30 s. Two goals: (1) reduce total pixel-on-time to a tiny fraction so the
// panel barely glows even with the contrast register pinned, (2) prevent any
// single pixel from accumulating wear. noinline keeps oled_loop's literal pool
// in Thumb's 4 KB reach (same constraint the other render_screen_* hit).
__attribute__((noinline))
void render_dim_pulse(uint32_t dim_elapsed_us) {
    fb_clear();
    constexpr uint32_t kPulsePeriodUs = 2UL * 1000000UL; // 2 s blink cycle
    constexpr uint32_t kPulseOnUs     = 1UL * 1000000UL; // 1 s on, 1 s off
    constexpr uint32_t kPosStepUs     = 30UL * 1000000UL; // 30 s per position
    constexpr int kPositions[][2] = {
        { 16,  8}, { 64,  8}, {112,  8},
        {112, 32},
        {112, 56}, { 64, 56}, { 16, 56},
        { 16, 32},
    };
    constexpr int kNumPositions = sizeof(kPositions) / sizeof(kPositions[0]);
    const bool dot_on = (dim_elapsed_us % kPulsePeriodUs) < kPulseOnUs;
    if (dot_on) {
        const int idx = (int)((dim_elapsed_us / kPosStepUs) % (uint32_t)kNumPositions);
        const int cx = kPositions[idx][0];
        const int cy = kPositions[idx][1];
        // 2x2 dot — small enough to barely register, big enough to see across a desk.
        rect_filled(cx, cy, 2, 2);
    }
    flush_fb_raw(); // skip chrome arrows; nothing to navigate to from sleep
}

void oled_loop() {
    handle_buttons();
    const uint32_t now = time_us_32();
    rumble_burst_tick(now);
    if ((now - last_render_us) < kFrameUs) return;
    last_render_us = now;
    // Track charge progress every frame — before the power-ladder early-returns
    // below, so step timing stays correct even while the panel is dimmed/off.
    sample_charge_eta();
    // Parse the DS5's per-unit IMU calibration once it lands (no-op until then),
    // so the tilt screen + tilt->RGB lightbar use corrected accel. See imu_apply().
    imu_cal_service();
    // Drive the controller LED every frame (any screen / power state): charging
    // pulse, selected OLED mode, or hand-off to the host. See lightbar_service().
    lightbar_service();
    // Bump activity on controller input changes (cheap rolling hash over input
    // bytes). Mirror bt.cpp's inactivity heuristic so resting-controller noise
    // doesn't read as activity: the analog sticks (idata[0..3]) jitter by ±1 LSB
    // at rest, so collapse their rest band [120,140] to a constant, and skip
    // idata[6] (the volatile counter byte bt.cpp's idle check also ignores).
    // Without this the dot/dim tier never engages while a controller is
    // connected, because a stick flicker resets the idle timer every few frames.
    uint32_t hash = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 6) continue;
        uint8_t b = interrupt_in_data[i];
        if (i < 4 && b >= 120 && b <= 140) b = 128; // stick deadzone
        hash = hash * 31u + b;
    }
    if (hash != last_input_hash) {
        last_input_hash = hash;
        // Controller input only keeps the panel awake when the user has left
        // "CtrlWake" on (the default). With it off, the dim/off timers count
        // down during gameplay and only KEY0/KEY1 wake the screen — see
        // handle_buttons(), which bumps last_activity_us unconditionally.
        // Issues #8 / #9.
        if (get_config().controller_wakes_display) last_activity_us = time_us_64();
    }
    // Rising-edge: BT-connect itself counts as activity, so the screen wakes
    // the moment a controller pairs rather than waiting for the first input.
    const bool bt_connected_now = bt_is_connected();
    if (bt_connected_now && !prev_bt_connected) last_activity_us = time_us_64();
    prev_bt_connected = bt_connected_now;

    // Power-state ladder: Active → Dim (breathing dot) → Off based on idle time.
    // Thresholds are user-configurable (minutes; 0 = that tier disabled) — #5.
    // While charging we cap the ladder at Dim — the panel keeps doing the
    // low-power breathing dot but never fully sleeps. This stops the user from
    // unplugging the controller just to "wake" the dongle (which would reset the
    // charge-ETA calibration). The dot tier already draws ~no current, so this
    // costs little; sample_charge_eta() runs before this block regardless.
    const uint64_t idle   = time_us_64() - last_activity_us;
    const uint64_t dim_us = (uint64_t)get_config().screen_dim_timeout * 60ULL * 1000000ULL;
    const uint64_t off_us = (uint64_t)get_config().screen_off_timeout * 60ULL * 1000000ULL;
    const bool off_enabled = get_config().screen_off_timeout != 0;
    const bool dim_enabled = get_config().screen_dim_timeout != 0;
    if (off_enabled && idle > off_us && !g_charge_eta.charging) {
        if (oled_power_state != OLED_OFF) {
            cmd(0xAE);
            oled_power_state = OLED_OFF;
        }
        return; // panel is off, nothing to draw
    }
    if (oled_power_state == OLED_OFF) cmd(0xAF); // wake panel before drawing
    if (dim_enabled && idle > dim_us) {
        sh1107_set_contrast(kDimContrast);
        oled_power_state = OLED_DIM;
        render_dim_pulse((uint32_t)(idle - dim_us));
        return; // skip the regular per-screen render path
    }
    sh1107_set_contrast(kBrightLevels[bright_idx]);
    oled_power_state = OLED_ACTIVE;

    // Ohad fixed19: temporary full-screen popup. Keep it after the power-ladder
    // wake/dim logic so it also wakes the OLED if the panel was sleeping.
    if (oled_popup_msg[0] && (int32_t)((uint32_t)time_us_32() - oled_popup_until_us) < 0) {
        fb_clear();
        auto cx_for = [](const char* t) {
            int n = 0; while (t[n]) n++;
            return (128 - (n * 6 - 1)) / 2;
        };
        if (ui_hebrew()) {
            const int w = hebrew_text_width(oled_popup_msg);
            int right = (128 + w) / 2;
            if (right > 126) right = 126;
            if (right < w) right = w;
            draw_hebrew_r(right, 28, oled_popup_msg);
        } else {
            draw_text(cx_for(oled_popup_msg), 28, oled_popup_msg);
        }
        flush_fb();
        return;
    }
    if (oled_popup_msg[0] && (int32_t)((uint32_t)time_us_32() - oled_popup_until_us) >= 0) {
        oled_popup_msg[0] = '\0';
    }

    // True on the first render after navigating to a different screen.
    // Lets a screen do expensive one-shot work on entry (the CPU screen
    // caches its frequency-counter measurement here).
    static int last_rendered_screen = -1;
    const bool screen_entered = (current_screen != last_rendered_screen);

    // Leaving Trigger Test in either direction → reset the adaptive
    // trigger preset to OFF and push it to the controller. Otherwise
    // the last-cycled effect (Weapon snap, Galloping pulse, etc.)
    // stays active on the DS5 indefinitely, which surprised users
    // who'd just navigated away expecting a clean slate.
    if (last_rendered_screen == kScreenTriggers
        && current_screen != kScreenTriggers) {
        trigger_preset = 0;
        send_trigger_effect(0);
    }

    // Leaving the Lightbar screen → persist mode/favorite changes made there,
    // batched into a single flash write instead of one per button press.
    if (last_rendered_screen == kScreenLightbar
        && current_screen != kScreenLightbar
        && lb_dirty) {
        lightbar_save_config();
    }

    last_rendered_screen = current_screen;

    switch (current_screen) {
        case kScreenStatus:   render_screen();           break;
        case kScreenSlots:    render_screen_slots();     break;
        case kScreenLightbar: render_screen_lightbar();  break;
        case kScreenTriggers: render_screen_triggers();  break;
        case kScreenGyro:     render_screen_gyro();      break;
        case kScreenTouchpad: render_screen_touchpad();  break;
        case kScreenRssi:     render_screen_rssi();      break;
        case kScreenVU:       render_screen_vu();        break;
        case kScreenRemap:    render_screen_remap();     break;
        case kScreenHelp:     render_screen_help();      break;
        case kScreenSettings: render_screen_settings();  break;
    }
}
