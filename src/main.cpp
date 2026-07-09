//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include "bsp/board_api.h"
#include "bt.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "state_mgr.h"
#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "config.h"
#include "cmd.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif
#include "oled.h"
#include "remap.h"
#include "midi_pt.h"

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"

int reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;
// fixed65ac: true only while the USB Audio microphone streaming interface is open.
// A real wired DualSense should not start sending microphone audio just because
// WebHID opened the gamepad; the host must select the mic AS interface alt setting.
volatile bool usb_mic_stream_active = false;

// Mic-debug instrumentation: count every 0x31 BT input report regardless
// of mic-tag bit, accumulate OR-mask of every byte-2 value seen (tells us
// which bits ever fire) and remember the last byte-2 value. Also track
// observed frame-length range. Surfaced on the OLED Diagnostics screen.
volatile uint32_t g_bt_31_packets = 0;
volatile uint32_t g_bt_other_packets = 0;
volatile uint8_t  g_last_other_id = 0;
volatile uint8_t  g_other_id_or = 0;
volatile uint8_t  g_last_31_b2 = 0;
volatile uint8_t  g_31_b2_or = 0;
volatile uint16_t g_31_len_min = 0xFFFF;
volatile uint16_t g_31_len_max = 0;
volatile uint8_t  g_mic_prefix[6] = {0};
volatile uint8_t  g_last_other_prefix[8] = {0};
volatile uint8_t  g_last_any_prefix[16] = {0};
volatile uint16_t g_longest_len = 0;
volatile uint8_t  g_longest_frame[80] = {0};
uint32_t bt_31_packet_count() { return g_bt_31_packets; }
uint8_t  bt_31_last_byte2()  { return g_last_31_b2; }
uint8_t  bt_31_b2_or_mask()  { return g_31_b2_or; }
uint16_t bt_31_len_min()     { return g_31_len_min == 0xFFFF ? 0 : g_31_len_min; }
uint16_t bt_31_len_max()     { return g_31_len_max; }
void bt_31_mic_prefix(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = g_mic_prefix[i];
}

// Trigger-flow diagnostics. Counts host → dongle → BT path for adaptive
// trigger effects. Lets us tell which link in the chain breaks when games
// like Death Stranding 2 don't produce trigger tension via the dongle:
//   out02_total     - every 0x02 HID OUT report received from host
//   out02_trig_allow - of those, how many set AllowRight/LeftTriggerFFB
//                     (valid_flag0 bits 2 & 3) — i.e. the host actually
//                     told us "apply trigger FFB"
//   out02_to_bt     - 0x02 reports that we forwarded to the controller as
//                     a BT 0x31 sub-0x10 packet (gated off when speaker is
//                     active; audio.cpp's 0x36 path carries state then)
//   out02_trig_folded - of the trig_allow reports, how many arrived while the
//                     speaker stream was active and were therefore NOT sent as
//                     a standalone 0x31 — their trigger FFB was folded into the
//                     0x36 audio frames via state[]. So trig_allow == to_bt's
//                     trigger share + this, proving the "missing" forwards
//                     (issue #6) aren't drops. Surfaced on the Diag screen.
// Surfaced on the OLED Diagnostics screen.
volatile uint32_t g_host_out02_total = 0;
volatile uint32_t g_host_out02_trig_allow = 0;
volatile uint32_t g_host_out02_to_bt = 0;
volatile uint32_t g_host_out02_trig_folded = 0;
uint32_t host_out02_total()       { return g_host_out02_total; }
uint32_t host_out02_trig_allow()  { return g_host_out02_trig_allow; }
uint32_t host_out02_to_bt()       { return g_host_out02_to_bt; }
uint32_t host_out02_trig_folded() { return g_host_out02_trig_folded; }

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

critical_section_t report_cs;
volatile bool report_dirty = false;

static void send_current_state_to_controller() {
    if (!bt_is_connected()) return;
    uint8_t outputData[78]{};
    outputData[0] = 0x31;
    outputData[1] = reportSeqCounter << 4;
    if (++reportSeqCounter == 256) reportSeqCounter = 0;
    outputData[2] = 0x10;
    state_set(outputData + 3, sizeof(SetStateData));
    bt_write(outputData, sizeof(outputData));
}

static void send_audio_safe_state_before_flash_poweroff() {
    // 1.0.4 audio hotfix:
    // If AudioKeep left USB/BT audio active and the user then saves settings
    // and powers off, a forced flash write could happen while the DualSense
    // still thinks its audio route is active. Send one explicit audio-muted /
    // powersave state first, then wait a short window before the flash write.
    if (!bt_is_connected()) return;
    uint8_t outputData[78]{};
    outputData[0] = 0x31;
    outputData[1] = reportSeqCounter << 4;
    if (++reportSeqCounter == 256) reportSeqCounter = 0;
    outputData[2] = 0x10;
    state_set(outputData + 3, sizeof(SetStateData));

    uint8_t *d = outputData + 3;
    // SetStateData byte layout used by state_mgr.cpp/audio.cpp:
    // d[0] flags: allow headphone volume, speaker volume, audio control.
    // d[1] flags: allow audio mute/control2.
    // d[4]/d[5]: headphone/speaker volume. d[7]: output path.
    // d[9]: audio mute control. d[37]: audio control2 / speaker preamp.
    d[0] |= 0x10 | 0x20 | 0x80;
    d[1] |= 0x02 | 0x80;
    d[4] = 0x00;       // headphones volume off
    d[5] = 0x00;       // speaker volume off
    d[7] = 0x30;       // neutral/default speaker path
    d[9] = 0x20 | 0x40; // mute speaker + headphones
    d[37] = 0x00;      // speaker preamp/control2 off
    bt_write(outputData, sizeof(outputData));
}

// fixed65q: safe PS+Options power-off based on fixed65n.
// Do not call bt_disconnect() directly from interrupt_loop(); it can race
// pending OLED/settings/remap/lightbar flash saves and sometimes reset before
// changes are persisted. This small state machine commits pending UI edits,
// gives the flash/OLED path a short quiet window, then disconnects BT.
static bool g_poweroff_pending = false;
static bool g_poweroff_disconnect_sent = false;
static uint32_t g_poweroff_save_at_us = 0;
static uint32_t g_poweroff_disconnect_at_us = 0;

// fixed65y: visible to the audio/HID paths so they can stop sending BT/audio
// packets during the local safe-poweroff window.
bool controller_poweroff_is_pending() {
    return g_poweroff_pending;
}

void controller_poweroff_request() {
    if (g_poweroff_pending) return;
    g_poweroff_pending = true;
    g_poweroff_disconnect_sent = false;
    g_poweroff_save_at_us = (uint32_t)time_us_32() + 300000u;
    g_poweroff_disconnect_at_us = 0;
    // AudioKeep can leave live audio running when the user saves settings and
    // immediately powers off. Do NOT start the BT output guard before this
    // packet, because the guard flushes/drops queued output. First send one
    // explicit audio-safe state, then the service routine waits ~300 ms before
    // touching flash.
    send_audio_safe_state_before_flash_poweroff();
    oled_return_to_status_screen();
    oled_show_message("Powering Off...", 1000);
}

void controller_poweroff_note_bt_disconnected() {
    // Keep the safe-poweroff guard active until the real HCI disconnect event.
    // Touchpad activity can keep high-rate interrupt reports flowing while the
    // shutdown is in progress; if we clear the guard immediately after sending
    // hci_disconnect(), those reports can race the teardown path and trip the
    // watchdog. The BT layer calls this from HCI_EVENT_DISCONNECTION_COMPLETE.
    g_poweroff_pending = false;
    g_poweroff_disconnect_sent = false;
    g_poweroff_save_at_us = 0;
    g_poweroff_disconnect_at_us = 0;
}

static void controller_poweroff_service() {
    if (!g_poweroff_pending) return;

#if !ENABLE_SERIAL
    // DS5Dongle 1.0.4 PoweroffWatchdogFix:
    // A planned controller power-off can spend longer than the original 1 s
    // watchdog window inside BT teardown after a long audio/game session. Keep
    // the Pico watchdog fed while the safe power-off state machine is active;
    // the watchdog remains enabled for real hangs elsewhere.
    watchdog_update();
#endif

    if (g_poweroff_disconnect_sent) {
        // Waiting for HCI_EVENT_DISCONNECTION_COMPLETE. Do not send another
        // disconnect command; just keep the watchdog alive while BTStack drains.
        return;
    }

    if (g_poweroff_disconnect_at_us == 0) {
        if ((int32_t)((uint32_t)time_us_32() - g_poweroff_save_at_us) < 0) {
            return;
        }
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        // Now that the controller has had time to receive the audio-safe state,
        // freeze new output packets and commit pending flash writes.
        bt_output_guard_start_ms(700);
        bt_flush_send_fifo();
        const bool saved_ok = oled_save_pending_changes_for_poweroff();
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        oled_show_message(saved_ok ? "Powering Off..." : "Save FAIL", saved_ok ? 700 : 1200);
        g_poweroff_disconnect_at_us = (uint32_t)time_us_32() + 250000u;
        return;
    }

    if ((int32_t)((uint32_t)time_us_32() - g_poweroff_disconnect_at_us) >= 0) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        bt_flush_send_fifo();
        bt_disconnect();
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        state_clear_host_audio_route();
        // Do not clear g_poweroff_pending here. Keep the guard active until the
        // BT stack confirms disconnection, so late touchpad/input reports are
        // ignored during teardown instead of racing watchdog-sensitive paths.
        g_poweroff_disconnect_sent = true;
    }
}

static void handle_pico_mic_button_toggle() {
    // fixed48: keep fixed44 Remap/PicoMic target, but make the local mic toggle
    // conservative. Crashes/stutter in fixed44+ were likely aggravated by flash
    // writes / repeated edge jitter while pressing Mute/PicoMic.
    static bool prev_action = false;
    static uint32_t last_toggle_us = 0;
    constexpr uint8_t kPsBit = 0x01;
    constexpr uint32_t kDebounceUs = 650000; // strong debounce: 650 ms

    const uint32_t now = time_us_32();
    const bool ps = (interrupt_in_data[9] & kPsBit) != 0;
    const bool action = remap_pico_mic_pressed(interrupt_in_data);

    // Rising edge only, with long debounce. Do not toggle while PS is held so
    // PS+Mute reboot remains usable. Do not save to flash on gameplay button
    // presses; this is runtime-only like a normal controller mute key.
    if (action && !prev_action && !ps && (uint32_t)(now - last_toggle_us) > kDebounceUs) {
        Config_body c = get_config();
        c.bt_mic_enable = c.bt_mic_enable ? 0 : 1;
        set_config(c);
        state_set_mute_light(!c.bt_mic_enable);
        send_current_state_to_controller();
        oled_show_message(c.bt_mic_enable ? "Pico Mic On" : "Pico Mic Off", 700);
        last_toggle_us = now;
    }
    prev_action = action;
}

void interrupt_loop() {
    // fixed65y: PS+Mute watchdog reboot shortcut removed.
    // A wired DualSense does not expose a PS+Mute dongle-reset combo, and keeping
    // this block before the poweroff guard let an accidental/stale Mute bit turn
    // a normal controller shutdown into a Pico watchdog reboot.

    // While the local power-off state machine is running, swallow controller
    // input for a few frames so PS+Options is not forwarded to the USB host and
    // the combo cannot retrigger before the HCI disconnect is sent.
    if (g_poweroff_pending) return;

    if (config_usb_midi_mode()) {
        // MIDI USB mode: the host sees only a class-compliant USB MIDI device.
        // No HID gamepad reports and no USB audio are forwarded in this mode.
        oled_handle_controller_screen_nav_shortcut();
        midi_pt_loop();
        return;
    }

    // Ohad fixed44: any button mapped to PicoMic toggles the Pico BT mic option.
    // Dedicated Mute button defaults to PicoMic. LED ON = Pico mic OFF.
    handle_pico_mic_button_toggle();

    // Ohad fixed19/fixed38: optional PS + Options within 100 ms powers
    // off/disconnects the controller. Disabled by default; enable from OLED
    // Settings as PowerCombo. Uses raw incoming report BEFORE remap.
    // Byte 8 bit 5 = Options, byte 9 bit 0 = PS/Home.
    if (get_config().power_combo_enable) {
        static bool prev_ps = false;
        static bool prev_options = false;
        static bool fixed19_poweroff_combo_fired = false;
        static uint32_t ps_down_us = 0;
        static uint32_t options_down_us = 0;
        constexpr uint32_t kPowerComboWindowUs = 100000;
        constexpr uint8_t kOptionsBit = 0x20;
        constexpr uint8_t kPsBit = 0x01;

        const uint32_t now = time_us_32();
        const bool ps = (interrupt_in_data[9] & kPsBit) != 0;
        const bool options = (interrupt_in_data[8] & kOptionsBit) != 0;

        if (ps && !prev_ps) ps_down_us = now;
        if (options && !prev_options) options_down_us = now;

        if (ps && options && !fixed19_poweroff_combo_fired && ps_down_us != 0 && options_down_us != 0) {
            const uint32_t diff = (ps_down_us > options_down_us)
                                      ? (ps_down_us - options_down_us)
                                      : (options_down_us - ps_down_us);
            if (diff <= kPowerComboWindowUs) {
                fixed19_poweroff_combo_fired = true;
                controller_poweroff_request();
                prev_ps = ps;
                prev_options = options;
                return; // do not forward the combo itself to the host
            }
        }

        if (!ps || !options) fixed19_poweroff_combo_fired = false;
        prev_ps = ps;
        prev_options = options;
    }

    // fixed65s: global physical Options + D-pad Left/Right pages OLED screens.
    // Pass-through mode: the shortcut updates the local OLED screen, but the
    // same physical input is still forwarded to the USB host. This keeps all
    // controller buttons visible to games/apps even while using the OLED UI.
    oled_handle_controller_screen_nav_shortcut();

    if (!tud_hid_ready()) return;

    // TODO: Refactor for better code reuse
    if (get_config().polling_rate_mode != 2) {
        // Remap acts on the OUTGOING copy only — interrupt_in_data stays raw so
        // the reboot combo above and every OLED screen keep seeing physical input.
        uint8_t out[63];
        memcpy(out, interrupt_in_data, 63);
        remap_apply(out);
        if (!tud_hid_report(0x01, out, 63)) {
            printf("[USBHID] tud_hid_report error\n");
        }
        return;
    }

    bool should_send = false;
    // Local buffer to hold the report data while we prepare it to send. 
    uint8_t safe_report[63];


    critical_section_enter_blocking(&report_cs);
    if (report_dirty) {
        memcpy(safe_report, interrupt_in_data, 63);
        report_dirty = false;
        should_send = true;
    }
    critical_section_exit(&report_cs);

    // Remap the snapshot, not interrupt_in_data (outgoing copy only — see above).
    if (should_send) {
        remap_apply(safe_report);
    }

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, 63)) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            critical_section_enter_blocking(&report_cs);
            report_dirty = true;
            critical_section_exit(&report_cs);
        }
    }
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    // Track ALL INTERRUPT input reports, not just 0x31. The mic stream
    // may live on a different report ID — confirmed 2026-05-19 that data[2]
    // bit 0 (and bit 1) is NOT a mic flag, just the report-type indicator;
    // every "mic-tagged" frame turned out to be standard input.
    if (channel == INTERRUPT && len > 1) {
        if (data[1] == 0x31) g_bt_31_packets++;
        else {
            g_bt_other_packets++;
            g_last_other_id = data[1];
            g_other_id_or = (uint8_t)(g_other_id_or | data[1]);
            for (uint16_t i = 0; i < 8 && i < len; i++) {
                g_last_other_prefix[i] = data[i];
            }
        }
        if (len > 2) {
            g_last_31_b2 = data[2];
            g_31_b2_or = (uint8_t)(g_31_b2_or | data[2]);
        }
        if (len < g_31_len_min) g_31_len_min = len;
        if (len > g_31_len_max) g_31_len_max = len;
        for (uint16_t i = 0; i < 16 && i < len; i++) {
            g_last_any_prefix[i] = data[i];
        }

        // Capture the entire content of the longest 0x31 frame we've
        // seen. Long frames almost certainly carry the mic audio appended
        // after the standard 63-byte input report — this lets us look
        // at the trailing bytes directly via 0xFD diagnostic.
        if (data[1] == 0x31 && len > g_longest_len) {
            g_longest_len = len;
            for (uint16_t i = 0; i < 80 && i < len; i++) {
                g_longest_frame[i] = data[i];
            }
        }
    }

    // Mic-in tap (TEST): once the dongle asserts the mic-enable bit in the
    // outgoing 0x36 audio report (pkt[4] bit 0, see audio.cpp — awalol
    // confirmed this is the key), the DS5 streams its mic as a 71-byte Opus
    // packet at data+4 of a 0x31 report with bit 1 of data[2] set. Route those
    // to the mic decoder instead of treating them as a standard input report.
    // The length guard (4-byte header + 71-byte Opus) keeps a stray short
    // frame from over-reading. The diagnostic counters above still observe
    // these frames, so the Diag screen's data[2] OR-mask will show bit 1 set
    // once the enable bit takes effect.
    // A mic-tagged 0x31 frame carries Opus audio at data+4, NOT a standard input
    // report — so it must ALWAYS be diverted here (decoded when mic is on, dropped
    // when off), never fall through to the input handler below. Letting it through
    // would copy Opus bytes into interrupt_in_data and corrupt sticks/buttons.
    if (channel == INTERRUPT && data[1] == 0x31 && ((data[2] >> 1) & 1)
        && len >= 75) {
        // During safe poweroff the mic stream is just load. Dropping it avoids
        // decoder/FIFO/USB-audio work while the BT link is being torn down.
        // fixed65ac: mimic wired USB mic behavior. Do not decode/forward BT mic
        // audio unless the host has actually opened the USB microphone streaming
        // interface. WebHID/gamepad testers often open HID only; with Pico Mic ON
        // the old code still armed the DS5 BT mic stream and added load during
        // page init. Mic-tagged BT frames are still swallowed so Opus bytes never
        // corrupt the gamepad input report.
        if (!config_usb_midi_mode() && !g_poweroff_pending && usb_mic_stream_active && get_config().bt_mic_enable) {
            mic_add_queue(data + 4);
        }
        return;
    }

    if (channel == INTERRUPT && data[1] == 0x31) {
        // Ohad fixed21: track the full DualSense jack flags, not only
        // PluggedHeadphones. data+3 is the 63-byte controller report, so
        // report[53] == data[56] and report[54] == data[57].
        static bool jack_seen_once = false;
        static uint8_t last_jack53 = 0;
        static uint8_t last_jack54 = 0;
        const uint8_t jack53 = len > 56 ? data[56] : 0;
        const uint8_t jack54 = len > 57 ? data[57] : 0;
        if (!jack_seen_once || jack53 != last_jack53 || jack54 != last_jack54) {
            set_headset_state(jack53, jack54);
            if (jack_seen_once) {
                const bool hp = (jack53 & 0x01) != 0;
                const bool mic = (jack53 & 0x02) != 0;
                const bool ext = (jack54 & 0x01) != 0;
                if (hp && (mic || ext)) oled_show_message("Headset Mic", 900);
                else if (hp)          oled_show_message("Headphones", 900);
                else                  oled_show_message("Jack Out", 900);
            }
            jack_seen_once = true;
            last_jack53 = jack53;
            last_jack54 = jack54;
        }

        if (get_config().polling_rate_mode != 2) {
            memcpy(interrupt_in_data, data + 3, 63);
            if (config_usb_midi_mode()) midi_pt_note_report(data + 3, 63);
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }

        critical_section_enter_blocking(&report_cs);
        memcpy(interrupt_in_data, data + 3, 63);
        report_dirty = true;
        critical_section_exit(&report_cs);
        if (config_usb_midi_mode()) midi_pt_note_report(data + 3, 63);
#if ENABLE_BATT_LED
        battery_led_note_report();
#endif
    }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    // --- DualSense feature reports that Linux's hid_playstation reads at probe ---
    // Without valid answers the kernel never creates a gamepad device, so games
    // outside Steam Input (Heroic/Proton/native) see no controller. The host asks
    // only for the report DATA (reqlen = report_size - 1); usbhid prepends the
    // report-id byte itself. The two CRC'd reports validate crc32 over
    // [0xA3 feature-seed, report_id, data...] in the last 4 bytes.
    // hid_playstation (kernel) AND the game's native DualSense detection both read
    // 0x09 (pairing), 0x20 (firmware) and 0x05 (calibration). The KERNEL only checks
    // size + crc, but the GAME validates the actual CONTENT — so synthesized zeros
    // pass the kernel yet get rejected by the game (a ~156x GET retry storm, and no
    // native adaptive triggers). Serve the REAL controller data, which init_feature()
    // caches from the controller over BT (get_feature_data returns it incl. the
    // report-id at [0]). Fall back to a crc-valid synthetic answer ONLY when the
    // controller isn't linked yet (USB-enumeration probe before the BT link), so the
    // kernel still binds at that moment.
    if (report_id == 0x09 || report_id == 0x20 || report_id == 0x05) {
        if (reqlen == 0) return 0;
        std::vector<uint8_t> real = get_feature_data(report_id, reqlen);
        if (real.size() > 1) {                     // real cached response present
            uint16_t n = (uint16_t)(real.size() - 1);
            if (n > reqlen) n = reqlen;
            memcpy(buffer, real.data() + 1, n);
            return n;
        }
        memset(buffer, 0, reqlen);
        if (report_id == 0x09) {                   // not linked yet: MAC-only stub
            if (reqlen >= 6) bt_get_addr(buffer);
            return reqlen;
        }
        if (reqlen < 5) return 0;                  // 0x20 / 0x05 stub: zeros + valid crc32
        uint8_t tmp[2 + 64];
        tmp[0] = 0xA3; tmp[1] = report_id;
        memcpy(tmp + 2, buffer, reqlen - 4);
        uint32_t crc = crc32_seeded(tmp, (size_t)(2 + (reqlen - 4)), 0);
        buffer[reqlen - 4] = (uint8_t)(crc);
        buffer[reqlen - 3] = (uint8_t)(crc >> 8);
        buffer[reqlen - 2] = (uint8_t)(crc >> 16);
        buffer[reqlen - 1] = (uint8_t)(crc >> 24);
        return reqlen;
    }

    if (is_pico_cmd(report_id)) {
        return pico_cmd_get(report_id, buffer, reqlen);
    }

    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    if (!feature_data.empty()) {
        memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
    }

    return feature_data.empty() ? 0 : feature_data.size() - 1;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
    uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

    if (itf == 1) {
        printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
        const bool new_active = alt != 0;
        if (spk_active != new_active) {
            spk_active = new_active;
            audio_usb_speaker_interface_changed(new_active);
        } else {
            spk_active = new_active;
        }
    } else if (itf == 2) {
        printf("[AUDIO] Set interface Microphone to alternate setting %d\n", alt);
        const bool new_active = alt != 0;
        if (usb_mic_stream_active != new_active) {
            usb_mic_stream_active = new_active;
            audio_usb_microphone_interface_changed(new_active);
        } else {
            usb_mic_stream_active = new_active;
        }
    }

    return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void) itf;
    (void) report_type;

    if (is_pico_cmd(report_id)) {
        printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", buffer[0]);
        pico_cmd_set(report_id, buffer, bufsize);
        return;
    }

    // fixed65x USB-native OUT handling:
    // A real USB DualSense has one Output report, ID 0x02. Depending on the
    // host API/TinyUSB path it can arrive either as an interrupt OUT packet
    // with the report ID in buffer[0], or as a control/WebHID SET_REPORT where
    // report_id carries 0x02 and buffer starts at SetStateData byte 0.
    // Treat both as the same USB report, but keep the host side "instant":
    // always update our state[] first. Only the BT translation is guarded/
    // coalesced, because the BT controller is the fragile/slow side — not USB.
    const uint8_t *out02_payload = nullptr;
    uint16_t out02_size = 0;
    bool out02_from_control = false;
    const bool is_output_report = (report_type == HID_REPORT_TYPE_OUTPUT || report_type == 0);
    if (is_output_report && report_id == 0 && bufsize >= 1 && buffer[0] == 0x02) {
        out02_payload = buffer + 1;
        out02_size = bufsize - 1;
    } else if (is_output_report && report_id == 0x02) {
        out02_payload = buffer;
        out02_size = bufsize;
        out02_from_control = true;

        // Be tolerant if a future TinyUSB/WebHID path provides the report ID
        // inside the buffer too. Without this, every SetStateData field would
        // be shifted by one byte and audio/trigger flags become nonsense.
        if (out02_size >= 1 && out02_payload[0] == 0x02) {
            out02_payload++;
            out02_size--;
        }
    }

    if (out02_payload != nullptr) {
        g_host_out02_total++;

        // A real DS5 Output report 0x02 carries SetStateData. Ignore malformed
        // short reports silently so a browser probe cannot spam printf/watchdog.
        if (out02_size < sizeof(SetStateData)) {
            return;
        }
        if (out02_size > sizeof(SetStateData)) {
            out02_size = sizeof(SetStateData);
        }

        // valid_flag0 is byte 0 of SetStateData. Bits 2 & 3 are
        // AllowRight/LeftTriggerFFB.
        const bool trigger_report = out02_size > 0 && (out02_payload[0] & 0x0C);
        if (trigger_report) {
            g_host_out02_trig_allow++;
        }

        // USB-side behavior: accept the report immediately, even during the
        // BT connect guard. This matches a wired DualSense better than dropping
        // SET_REPORT entirely, and it lets WebHID audio-route changes be picked
        // up by the next 0x36 audio frame without needing a standalone 0x31.
        state_update(out02_payload, out02_size);

        // No BT controller yet: a real USB device would still accept the host
        // report. Keep the latest state locally and do not queue stale output.
        if (!bt_is_connected()) {
            return;
        }
        if (g_poweroff_pending) {
            if (trigger_report) g_host_out02_trig_folded++;
            return;
        }

        // fixed65ak: OUT burst guard removed. The USB-side state was already
        // accepted above; only the WebHID/control coalesce below remains to avoid
        // converting a page-init burst into many standalone BT 0x31 packets.
        const uint32_t now_us = time_us_32();

        if (audio_usb_active()) {
            // Not forwarded as a standalone 0x31 while there is real USB audio
            // packet flow — the trigger/route state written into state[] rides
            // the next 0x36 audio frame instead. Use effective audio activity,
            // not only the raw alt setting, so a stale browser/tester stream
            // cannot keep swallowing output reports after packets stop.
            if (trigger_report) g_host_out02_trig_folded++;
            return;
        }

        // WebHID/tester can send many control SET_REPORT(0x02) packets during
        // page init. A wired DualSense absorbs them over USB; our bridge would
        // otherwise convert each one into a BT 0x31 burst. Coalesce only the
        // control/WebHID form. Normal interrupt OUT from games keeps 65v latency.
        static uint32_t last_control_out02_bt_us = 0;
        constexpr uint32_t kControlOut02BtMinGapUs = 8000; // 125 Hz max
        if (out02_from_control && last_control_out02_bt_us != 0 &&
            (uint32_t)(now_us - last_control_out02_bt_us) < kControlOut02BtMinGapUs) {
            if (trigger_report) g_host_out02_trig_folded++;
            return;
        }
        if (out02_from_control) {
            last_control_out02_bt_us = now_us;
        }

        uint8_t outputData[78]{};
        outputData[0] = 0x31;
        outputData[1] = reportSeqCounter << 4;
        if (++reportSeqCounter == 256) {
            reportSeqCounter = 0;
        }
        outputData[2] = 0x10;
        state_set(outputData + 3, sizeof(SetStateData));
        bt_write(outputData, sizeof(outputData));
        g_host_out02_to_bt++;
        return;
    }
    if (report_id == 0x80 ||
        // DSE: Write Profile Block
        report_id == 0x60 ||
        report_id == 0x62 ||
        report_id == 0x61) {
        set_feature_data(report_id, const_cast<uint8_t *>(buffer), bufsize);
        return;
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    board_init();

    // Load persistent Settings before TinyUSB starts. USB Mode changes the
    // device/configuration descriptors, so it must be known before enumeration.
    config_load();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL
    tud_disconnect();
#endif
    board_init_after_tusb();
#if ENABLE_SERIAL
    stdio_usb_init();
#endif

    if (cyw43_arch_init()) {
        printf("Failed to initialize CYW43\n");
        return 1;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // DS5Dongle 1.0.5 FastWatchdog:
        // keep the watchdog indication short so recovery feels immediate.
        for (int i = 0; i < 6; i++) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (i % 2) == 0);
            sleep_ms(90);
        }
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    } else {
        printf("Clean boot\n");
    }
#endif

    // Initialize the critical section for the report buffer
    critical_section_init(&report_cs);

    remap_load();

    bt_init();
    bt_register_data_callback(on_bt_data);

    if (config_usb_midi_mode()) {
        midi_pt_init();
    } else {
        audio_init();
    }
    state_init();
    oled_init();
    if (config_usb_midi_mode()) {
        oled_show_message("MIDI Only", 1200);
    }

#if !ENABLE_SERIAL
    // DS5Dongle 1.0.5 FastWatchdog:
    // Keep the watchdog recovery fast. The power-off path feeds the watchdog
    // explicitly, so we do not need a long global timeout anymore.
    watchdog_enable(1200, true);
#endif

    while (1) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        cyw43_arch_poll();
        bt_connection_watchdog_tick();
        tud_task();
        if (!config_usb_midi_mode()) {
            audio_loop();
        }
        interrupt_loop();
        oled_loop();
        config_service_deferred_save();
        controller_poweroff_service();
#if ENABLE_BATT_LED
        battery_led_tick();
#endif
    }
}
