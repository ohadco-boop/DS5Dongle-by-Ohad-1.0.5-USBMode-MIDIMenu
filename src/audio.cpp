//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "pico/platform.h"
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/time.h"
#include "config.h"

// fixed65ac: set by TinyUSB audio interface callback in main.cpp.
extern bool spk_active;
extern volatile bool usb_mic_stream_active;
#include "state_mgr.h"
#include "usb.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48

// DualSense microphone, ported from awalol/DS5Dongle's `mic` branch.
// The DS5 sends mic audio as Opus packets embedded in BT input report
// 0x31 when bit 1 of byte 2 is set; payload is 71 bytes of Opus at
// offset 4, decoded to mono 48 kHz 10 ms frames (480 samples).
#define MIC_CHANNELS      1
#define MIC_FRAMES        480
#define MIC_OPUS_SIZE     71

using std::clamp;
using std::max;

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
static bool plug_mic = false;
static bool plug_external_mic = false;
static uint8_t jack_flags53 = 0;
static uint8_t jack_flags54 = 0;

// src/main.cpp safe-poweroff state. Audio must not keep the BT link busy while
// the local shutdown/disconnect state machine is running.
extern bool controller_poweroff_is_pending();

// fixed25: after headset plug/unplug, force several control packets even if
// mic packets are already streaming. This re-applies AudioControl so the DS5
// returns to internal mic after unplugging and switches to headset mic after
// plugging.
static volatile uint64_t g_mic_route_rearm_until_us = 0;

// 1.0.3 HeadsetBidirectionalRoute: a jack plug/unplug event on a real
// wired DualSense updates the physical audio path immediately. Some games
// stop sending audio/control packets right after hotplug, so the bridge must
// push a short control-only 0x36 refresh window itself. This is intentionally
// lighter than 1.0.2 HeadsetForce: it does not override host volume/mute, it
// just re-stamps the current fallback route (headset on plug, speaker on
// unplug) into SetStateData.
static volatile uint64_t g_jack_route_refresh_until_us = 0;

// 1.0.4 AudioRouteFix:
// TinyUSB's SET_INTERFACE tells us when the host opens/closes the UAC speaker
// stream, but browsers/test pages can leave the DS5 AudioControl route cached
// after they stop producing audio. A physical jack change used to be the only
// thing that reset that route. Track real USB speaker packet flow and, when the
// stream closes or goes packet-idle, drop stale host AudioControl and push the
// normal physical-jack fallback route back to the controller.
static volatile uint32_t g_usb_speaker_if_change_us = 0;
static volatile uint32_t g_usb_speaker_last_packet_us = 0;
static volatile bool     g_usb_speaker_idle_recovered = false;
static volatile bool     g_usb_mic_if_active = false;
static constexpr uint32_t USB_SPK_START_GRACE_US = 1200000u;
static constexpr uint32_t USB_SPK_IDLE_US        = 900000u;

static void audio_force_route_refresh_ms(uint32_t ms) {
    const uint64_t until = time_us_64() + (uint64_t)ms * 1000ULL;
    if ((int64_t)(until - g_jack_route_refresh_until_us) > 0) {
        g_jack_route_refresh_until_us = until;
    }
}

void audio_usb_speaker_interface_changed(bool active) {
    const uint32_t now = time_us_32();
    g_usb_speaker_if_change_us = now;
    g_usb_speaker_last_packet_us = 0;
    g_usb_speaker_idle_recovered = false;

    if (!active) {
        // Host closed the UAC speaker stream (for example after closing
        // DualSense Tester). Its last WebHID AudioControl may otherwise keep
        // the DS5 stuck on the tester-selected route until a jack hotplug.
        state_clear_host_audio_route();
        audio_force_route_refresh_ms(1500);
    } else {
        // Give a newly-opened stream a short route refresh window too. This
        // helps the occasional cold-start case where the DS5 did not receive a
        // clean initial route before audio begins.
        audio_force_route_refresh_ms(1000);
    }
}

void audio_usb_microphone_interface_changed(bool active) {
    g_usb_mic_if_active = active;
}

static bool audio_speaker_effectively_active() {
    if (!spk_active) return false;
    const uint32_t now = time_us_32();
    const uint32_t last = g_usb_speaker_last_packet_us;
    if (last != 0) {
        return (uint32_t)(now - last) < USB_SPK_IDLE_US;
    }
    // Treat a freshly-opened interface as active briefly, before the first
    // packet arrives, so AudioKeep/config-save logic does not race startup.
    const uint32_t opened = g_usb_speaker_if_change_us;
    return opened != 0 && (uint32_t)(now - opened) < USB_SPK_START_GRACE_US;
}

static bool audio_microphone_effectively_active() {
    // USB microphone is an IN stream, so there is no host OUT packet cadence to
    // observe. If the host selected the mic alternate setting, treat it as
    // active until TinyUSB reports it closed.
    return usb_mic_stream_active || g_usb_mic_if_active;
}

static void audio_route_recovery_service() {
    if (controller_poweroff_is_pending()) return;
    if (!bt_is_connected()) return;
    if (!spk_active) return;

    const uint32_t now = time_us_32();
    const uint32_t last = g_usb_speaker_last_packet_us;
    const uint32_t opened = g_usb_speaker_if_change_us;
    bool packet_idle = false;

    if (last != 0) {
        packet_idle = (uint32_t)(now - last) >= USB_SPK_IDLE_US;
    } else if (opened != 0) {
        packet_idle = (uint32_t)(now - opened) >= USB_SPK_START_GRACE_US;
    }

    if (packet_idle && !g_usb_speaker_idle_recovered) {
        // No more USB speaker packets while the interface is still marked open:
        // recover exactly like a jack route refresh, without adding diagnostics
        // or a user-visible workaround.
        state_clear_host_audio_route();
        audio_force_route_refresh_ms(1500);
        g_usb_speaker_idle_recovered = true;
    } else if (!packet_idle) {
        g_usb_speaker_idle_recovered = false;
    }
}
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
static uint8_t opus_buf[200];
critical_section_t opus_cs;

// Mic ingress queue — filled from on_bt_data() (BT poll, core0), drained
// at the top of audio_loop() on core0. The decoder is single-threaded
// (core0 only), so no critical section is needed around it.
queue_t mic_fifo;
struct mic_element { uint8_t data[MIC_OPUS_SIZE]; };
static OpusDecoder *mic_decoder = nullptr;
static volatile uint32_t g_mic_frames = 0;
static volatile int32_t  g_mic_last_decoded = 0;  // opus_decode return value
static volatile uint16_t g_mic_last_want = 0;     // bytes we asked TinyUSB to send
static volatile uint16_t g_mic_last_wrote = 0;    // bytes TinyUSB accepted
uint32_t audio_mic_frames() { return g_mic_frames; }
int32_t  audio_mic_last_decoded() { return g_mic_last_decoded; }
uint16_t audio_mic_last_want()    { return g_mic_last_want; }
uint16_t audio_mic_last_wrote()   { return g_mic_last_wrote; }

// Mic jitter buffer + packet-loss concealment. Decoded mono frames land here
// (filled as Opus arrives, drained at a steady 10 ms playout cadence) so bursty
// BT delivery is smoothed and a dropped frame is concealed via Opus PLC instead
// of underrunning the host with a click/hole. Design ported from
// SundayMoments/DS5_Bridge (credit there). PLC keeps voice continuous on a
// lossy BT link (e.g. controller moved away, USB 3.0 RF interference).
struct mic_decoded_element { int16_t mono[MIC_FRAMES]; };
static queue_t mic_decode_fifo;
static constexpr int      MIC_DECODE_DEPTH  = 8;       // jitter-buffer capacity (frames)
static constexpr int      MIC_PLAYOUT_START = 3;       // pre-buffer before playout begins
static constexpr uint64_t MIC_FRAME_US      = 10000;  // 10 ms per Opus frame @ 48 kHz
static constexpr uint64_t MIC_SESSION_US    = 300000; // no real frame this long → stop playout
static bool     mic_playout_started = false;
static uint64_t mic_next_playout_us = 0;
static uint64_t mic_last_real_us    = 0;
static volatile uint32_t g_mic_plc_frames = 0;        // concealed frames generated (Diag)
uint32_t audio_mic_plc_frames() { return g_mic_plc_frames; }

struct audio_raw_element {
    float data[512 * 2];
};

void set_headset(bool state) {
    set_headset_state(state ? 0x01 : 0x00, 0x00);
}

void set_headset_state(uint8_t flags53, uint8_t flags54) {
    static bool initialized = false;
    const bool old_route = plug_headset || plug_mic || plug_external_mic;

    jack_flags53 = flags53;
    jack_flags54 = flags54;
    plug_headset = (flags53 & 0x01) != 0;
    plug_mic = (flags53 & 0x02) != 0;
    plug_external_mic = (flags54 & 0x01) != 0;
    const bool new_route = plug_headset || plug_mic || plug_external_mic;

    // Force external mic routing as soon as the 3.5mm jack is detected. Some
    // DualSense firmwares only raise PluggedHeadphones initially and set the
    // external-mic status after AudioControl is changed, so use headphones as
    // the trigger for the route.
    state_set_external_mic_route(new_route);

    const uint64_t now = time_us_64();
    g_mic_route_rearm_until_us = now + 3000000ULL; // 3.0s route re-arm window

    // 1.0.3: on both plug and unplug, push a few control-only route packets.
    // This makes unplug return to the controller speaker even if the game does
    // not immediately send another DualSense audio control report.
    if (!initialized || old_route != new_route) {
        g_jack_route_refresh_until_us = now + 1500000ULL; // 1.5s route refresh window
    }
    initialized = true;
}

bool audio_headphones_plugged() { return plug_headset; }
bool audio_headset_mic_plugged() { return plug_mic; }
bool audio_external_mic_active() { return plug_external_mic; }
uint8_t audio_jack_flags53() { return jack_flags53; }
uint8_t audio_jack_flags54() { return jack_flags54; }

// Stubs kept for OLED diag-screen compatibility. Upstream removed the opus
// queue and audio FIFO drop tracking isn't wired here; OLED shows 0.
uint32_t audio_fifo_drops() { return 0; }
uint32_t opus_fifo_drops() { return 0; }

// Monotonic byte-flow counters for the OLED Diagnostics screen and the web
// emulator's USB / BT rate display. Updated below.
static volatile uint32_t g_usb_frames = 0;
static volatile uint32_t g_bt_packets = 0;
uint32_t audio_usb_frames() { return g_usb_frames; }
bool audio_usb_active() { return audio_speaker_effectively_active() || audio_microphone_effectively_active(); }
uint32_t audio_bt_packets() { return g_bt_packets; }

// Rolling-peak meters for the OLED VU screen. Updated during audio_loop's
// per-sample iteration, decayed 12.5 % on each read (so the bar falls back
// over a few frames if the signal goes quiet).
static volatile uint16_t g_peak_spk = 0;
static volatile uint16_t g_peak_hap = 0;
uint8_t audio_peak_speaker() {
    const uint16_t v = g_peak_spk;
    g_peak_spk = (uint16_t)((v * 7) / 8);
    return (uint8_t)(v >> 7);
}
uint8_t audio_peak_haptic() {
    const uint16_t v = g_peak_hap;
    g_peak_hap = (uint16_t)((v * 7) / 8);
    return (uint8_t)(v >> 7);
}

// Most-recent Opus TOC byte (first byte of the packet). Used by the OLED
// Diagnostics screen to decode the frame's bandwidth + duration config
// without serial.
static volatile uint8_t g_mic_toc = 0;
uint8_t audio_mic_last_toc() { return g_mic_toc; }

// Push a 71-byte Opus mic packet from the BT handler into the mic_fifo.
// Called from src/main.cpp's on_bt_data() when the DS5 sends a mic-tagged
// 0x31 input report. Drops the oldest queued packet if the FIFO is full —
// preferring fresh audio over backlog on overload.
void __not_in_flash_func(mic_add_queue)(const uint8_t *data) {
    static mic_element packet{};
    memcpy(packet.data, data, MIC_OPUS_SIZE);
    g_mic_toc = data[0]; // first byte of the Opus packet
    // fixed65al: never remove into NULL on FIFO overflow. If the FIFO is full,
    // keep the buffered packet(s) and drop the newest packet. This avoids a
    // Pico hard fault during heavy startup bursts without adding RAM.
    queue_try_add(&mic_fifo, &packet);
}

// Re-assert the DS5 mic-enable (pkt[4] bit 0) so the controller streams its mic
// even when no audio is being output to it. Normally the enable only rides the
// 0x36 audio frames, which are gated on active USB audio — so without this, mic
// only works while a game plays sound. The enable is sticky (the DS5 keeps
// streaming once it starts), so we send a control-only 0x36 (enable + the
// load-bearing SetStateData sub-report + a silent haptic block, no speaker
// payload → makes no sound) at ~4 Hz ONLY until mic frames start arriving, then
// stop — minimizing BT traffic and DS5 battery. Resumes if the stream stalls.
static void mic_enable_keepalive() {
    if (controller_poweroff_is_pending()) return;
    if (!bt_is_connected() || !get_config().bt_mic_enable) return;
    if (!usb_mic_stream_active) return;
    const uint64_t now = time_us_64();
    static uint32_t last_frames = 0;
    static uint64_t last_frame_us = 0;
    static uint64_t last_send_us = 0;
    const uint32_t frames = g_mic_frames;
    if (frames != last_frames) { last_frames = frames; last_frame_us = now; }
    const bool route_rearm = (int64_t)(g_mic_route_rearm_until_us - now) > 0;
    if (!route_rearm && last_frame_us != 0 && (now - last_frame_us) < 1000000ULL) return; // streaming → sticky, no resend
    const uint64_t min_gap_us = route_rearm ? 100000ULL : 250000ULL;
    if (last_send_us != 0 && (now - last_send_us) < min_gap_us) return;    // throttle while arming/re-arming
    last_send_us = now;

    uint8_t pkt[REPORT_SIZE]{};
    pkt[0] = REPORT_ID;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 1 << 7;
    pkt[3] = 7;
    pkt[4] = 0b11111111; // mic-enable (bit 0)
    const auto buf_len = get_config().audio_buffer_length;
    pkt[5] = pkt[6] = pkt[7] = pkt[8] = pkt[9] = buf_len;
    pkt[10] = packetCounter++;
    pkt[11] = 0x10 | 1 << 7; // SetStateData sub-report (load-bearing — keeps actuators alive)
    pkt[12] = 63;
    state_set(pkt + 13, 63);
    pkt[76] = 0x12 | 1 << 7;  // haptic sub-report; samples left zero = silent
    pkt[77] = SAMPLE_SIZE;
    // no speaker sub-report (pkt[142..] stays zero) → control-only, no audio out
    bt_write(pkt, sizeof(pkt));
    g_bt_packets++;
}


// 1.0.3 HeadsetBidirectionalRoute: after jack plug/unplug, send a few
// control-only 0x36 packets carrying the updated SetStateData route. This is
// what makes unplug reliably return to the internal controller speaker without
// waiting for the game/host to rediscover the jack state.
static void jack_route_refresh_keepalive() {
    if (controller_poweroff_is_pending()) return;
    if (!bt_is_connected()) return;

    const uint64_t now = time_us_64();
    if ((int64_t)(g_jack_route_refresh_until_us - now) <= 0) return;

    static uint64_t last_send_us = 0;
    if (last_send_us != 0 && (now - last_send_us) < 100000ULL) return; // 10 Hz
    last_send_us = now;

    uint8_t pkt[REPORT_SIZE]{};
    pkt[0] = REPORT_ID;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 1 << 7;
    pkt[3] = 7;
    // Keep mic state consistent, but do not arm BT mic just because jack route
    // changed. USB mic alt-gate still decides whether mic streaming is active.
    pkt[4] = (get_config().bt_mic_enable && usb_mic_stream_active) ? 0b11111111 : 0b11111110;
    const auto buf_len = get_config().audio_buffer_length;
    pkt[5] = pkt[6] = pkt[7] = pkt[8] = pkt[9] = buf_len;
    pkt[10] = packetCounter++;

    pkt[11] = 0x10 | 1 << 7;
    pkt[12] = 63;
    state_set(pkt + 13, 63);

    pkt[76] = 0x12 | 1 << 7;
    pkt[77] = SAMPLE_SIZE;
    // No speaker sub-report: route/control refresh only, no audible packet.
    bt_write(pkt, sizeof(pkt));
    g_bt_packets++;
}

void __not_in_flash_func(audio_loop)() {
    // fixed65y: once local safe-poweroff starts, stop all audio/mic BT traffic.
    // This prevents mic-on from continuing to queue/decode/emit 0x36 packets
    // during the 250 ms shutdown window.
    if (controller_poweroff_is_pending()) return;

    audio_route_recovery_service();
    jack_route_refresh_keepalive();

    // Mic-in path: pull one Opus packet from the BT-side FIFO, decode to
    // mono PCM, duplicate to stereo (our UAC1 endpoint declares 2 channels),
    // push to the host via tud_audio_write. Runs once per loop iteration so
    // it keeps up with the ~100 Hz arrival rate of mic-tagged BT frames.
    if (mic_decoder != nullptr && usb_mic_stream_active) {
        const uint64_t now = time_us_64();

        // Decode stage: drain incoming Opus into the jitter buffer as fast as it
        // arrives (absorbs bursty BT delivery), up to the buffer's capacity.
        static mic_element pkt{};
        while (queue_get_level(&mic_decode_fifo) < MIC_DECODE_DEPTH
               && queue_try_remove(&mic_fifo, &pkt)) {
            static mic_decoded_element dec{};
            const int n = opus_decode(mic_decoder, pkt.data, MIC_OPUS_SIZE,
                                      dec.mono, MIC_FRAMES, 0);
            g_mic_last_decoded = n; // observed in OLED Diag
            if (n > 0) {
                queue_try_add(&mic_decode_fifo, &dec);
                mic_last_real_us = now;
            }
        }

        // Playout stage: emit one frame every 10 ms. Pre-buffer a few frames to
        // absorb jitter, then play a real frame if buffered, else conceal with an
        // Opus PLC frame during an active session (transient loss) so the host
        // hears continuity instead of a hole. If real frames have been gone for a
        // while (mic off/idle), stop so we don't emit comfort noise forever.
        if (!mic_playout_started
            && queue_get_level(&mic_decode_fifo) >= MIC_PLAYOUT_START) {
            mic_playout_started = true;
            mic_next_playout_us = now;
        }
        if (mic_playout_started && (int64_t)(now - mic_next_playout_us) >= 0) {
            static mic_decoded_element out{};
            bool have = queue_try_remove(&mic_decode_fifo, &out);
            if (!have) {
                if (now - mic_last_real_us < MIC_SESSION_US) {
                    const int n = opus_decode(mic_decoder, nullptr, 0,
                                              out.mono, MIC_FRAMES, 0); // PLC
                    if (n > 0) { have = true; g_mic_plc_frames++; }
                } else {
                    mic_playout_started = false; // session ended — re-buffer next time
                }
            }
            if (have) {
                static int16_t stereo[MIC_FRAMES * 2];
                const float mic_display_db = (float)((int)get_config().mic_gain_db_plus24 - 24);
                // DS5Dongle by Ohad 1.0.0 Stable: display 0 dB is calibrated
                // to the old fixed65am Mic Gain -20 dB level.
                const float mic_actual_db = mic_display_db - 20.0f;
                const float mic_gain = powf(10.0f, mic_actual_db / 20.0f);
                const float mic_scale = usb_mic_level_scale() * mic_gain;
                for (int i = 0; i < MIC_FRAMES; i++) {
                    // User Mic Gain from OLED menu (-24..+12 dB relative to the
                    // new 0 dB reference). Windows/UAC mic level still applies.
                    int32_t v = (int32_t)(out.mono[i] * mic_scale);
                    if (v < -32768) v = -32768;
                    if (v > 32767) v = 32767;
                    const int16_t mic_sample = (int16_t)v;
                    stereo[i * 2]     = mic_sample;
                    stereo[i * 2 + 1] = mic_sample;
                }
                const uint16_t want = (uint16_t)(MIC_FRAMES * 2 * sizeof(int16_t));
                g_mic_last_wrote = tud_audio_write(stereo, want);
                g_mic_last_want  = want;
                g_mic_frames++;
                mic_next_playout_us += MIC_FRAME_US;
                // Drift guard: if we've fallen many frames behind (loop stall),
                // resync the cadence instead of bursting to catch up.
                if ((int64_t)(now - mic_next_playout_us) > (int64_t)(4 * MIC_FRAME_US)) {
                    mic_next_playout_us = now + MIC_FRAME_US;
                }
            }
        }
    }

    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) {
        // Keep the DS5 mic streaming even without output audio — but ONLY once
        // the host has enumerated us (tud_mounted). Running it during the
        // fresh-pair feature handshake floods BT TX and delays controller-type
        // detection past the connection watchdog's timeout, which then tears the
        // link down (~10-15s "shutdown" on fresh pair). After enumeration the
        // handshake is done, so it's safe — and always-on mic still works.
        if (tud_mounted() && usb_mic_stream_active) mic_enable_keepalive();
        return;
    }

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }
    g_usb_frames += (uint32_t)frames;
    g_usb_speaker_last_packet_us = time_us_32();
    g_usb_speaker_idle_recovered = false;

    static float audio_buf[512 * 2];
    static uint audio_buf_pos = 0;
    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    // Ohad fixed63: Windows/UAC speaker volume is an always-on runtime multiplier.
    // Keep persistent firmware speaker_volume as the base trim; host volume
    // attenuates output audio only, not the haptic channels.
    const float audio_gain = powf(10.0f, get_config().speaker_volume / 20.0f) * usb_speaker_volume_gain();
    const float haptics_gain = get_config().haptics_gain;
    uint16_t spk_max = g_peak_spk;
    uint16_t hap_max = g_peak_hap;

    // ---- Audio Auto Haptics (borrowed from loteran/DS5Dongle 5d6bc2f) ----
    // Derives a haptic-feedback waveform from the speaker audio so games that
    // never write haptic data (e.g. Ghost of Tsushima on Linux+Steam) still
    // produce rumble. Mode 1 (Fallback, default) fires only when native is
    // silent → preserves native HD haptics in games that do send them.
    const uint8_t auto_mode = get_config().auto_haptics_enable;
    const float auto_gain   = (auto_mode > 0) ? (get_config().auto_haptics_gain / 100.0f) * haptics_gain : 0.0f;
    static const float LP_COEFF[4] = { 0.01039f, 0.02074f, 0.03095f, 0.05123f };
    const float lp_a = LP_COEFF[get_config().auto_haptics_lowpass & 3];
    static float lp_l = 0.0f, lp_r = 0.0f;
    static float env_l = 0.0f, env_r = 0.0f;
    constexpr float ENV_ATK = 0.40f;
    constexpr float ENV_REL = 0.025f;
    constexpr int     NATIVE_SILENT_TIMEOUT = 100;
    constexpr uint16_t NATIVE_THRESHOLD     = 256;
    static int native_silent_count = NATIVE_SILENT_TIMEOUT * 2;
    const bool fallback_active = (auto_mode == 1) && (native_silent_count >= NATIVE_SILENT_TIMEOUT);

    for (int i = 0; i < nframes; i++) {
        // VU peak tracking
        {
            int16_t sl = raw[i * INPUT_CHANNELS];
            int16_t sr = raw[i * INPUT_CHANNELS + 1];
            int16_t hl = raw[i * INPUT_CHANNELS + 2];
            int16_t hr = raw[i * INPUT_CHANNELS + 3];
            uint16_t a = (uint16_t)(sl < 0 ? -sl : sl);
            uint16_t b = (uint16_t)(sr < 0 ? -sr : sr);
            if (a > spk_max) spk_max = a;
            if (b > spk_max) spk_max = b;
            a = (uint16_t)(hl < 0 ? -hl : hl);
            b = (uint16_t)(hr < 0 ? -hr : hr);
            if (a > hap_max) hap_max = a;
            if (b > hap_max) hap_max = b;
        }
 #if !DISABLE_SPEAKER_PROC
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f * audio_gain;
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f * audio_gain;
        if (audio_buf_pos == 512 * 2) {
            static audio_raw_element element{};
            memcpy(element.data, audio_buf, 512 * 2 * 4);
            // fixed65al: Spider-Man can open the DualSense speaker/haptics stream
            // with a large startup burst. Do not call queue_try_remove(..., NULL)
            // when the audio FIFO is already full; just drop the newest block.
            if (!queue_try_add(&audio_fifo, &element)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            audio_buf_pos = 0;
        }
#endif
        float h_l = raw[i * INPUT_CHANNELS + 2] / 32768.0f * haptics_gain;
        float h_r = raw[i * INPUT_CHANNELS + 3] / 32768.0f * haptics_gain;

        if (auto_mode > 0) {
            const float spk_l = raw[i * INPUT_CHANNELS    ] / 32768.0f;
            const float spk_r = raw[i * INPUT_CHANNELS + 1] / 32768.0f;
            lp_l += lp_a * (spk_l - lp_l);
            lp_r += lp_a * (spk_r - lp_r);
            const float abs_l = lp_l < 0.0f ? -lp_l : lp_l;
            const float abs_r = lp_r < 0.0f ? -lp_r : lp_r;
            env_l = (abs_l > env_l) ? env_l + ENV_ATK * (abs_l - env_l)
                                    : env_l + ENV_REL * (abs_l - env_l);
            env_r = (abs_r > env_r) ? env_r + ENV_ATK * (abs_r - env_r)
                                    : env_r + ENV_REL * (abs_r - env_r);
            float al = lp_l * (1.0f + 3.0f * env_l) * auto_gain;
            float ar = lp_r * (1.0f + 3.0f * env_r) * auto_gain;
            al = al / (1.0f + (al < 0.0f ? -al : al));
            ar = ar / (1.0f + (ar < 0.0f ? -ar : ar));

            if (auto_mode == 3) {              // Replace
                h_l = al; h_r = ar;
            } else if (auto_mode == 2) {       // Mix
                float m_l = h_l + al, m_r = h_r + ar;
                h_l = m_l / (1.0f + (m_l < 0.0f ? -m_l : m_l));
                h_r = m_r / (1.0f + (m_r < 0.0f ? -m_r : m_r));
            } else if (auto_mode == 1 && fallback_active) {  // Fallback (default)
                h_l = al; h_r = ar;
            }
        }

        in_buf[i * 2]     = static_cast<WDL_ResampleSample>(clamp(h_l, -1.0f, 1.0f));
        in_buf[i * 2 + 1] = static_cast<WDL_ResampleSample>(clamp(h_r, -1.0f, 1.0f));
    }
    g_peak_spk = spk_max;
    g_peak_hap = hap_max;
    if (hap_max > NATIVE_THRESHOLD) {
        native_silent_count = 0;
    } else if (native_silent_count < NATIVE_SILENT_TIMEOUT * 2) {
        native_silent_count++;
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    const int out_frames = resampler.ResampleOut(out_buf, nframes, nframes / 4, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = static_cast<int>(out_buf[i * 2] * 127.0f);
        int val_r = static_cast<int>(out_buf[i * 2 + 1] * 127.0f);
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | 0 << 6 | 1 << 7;
        pkt[3] = 7;
        // bit 0 = mic-enable: tells the DS5 to stream its mic over BT (awalol
        // confirmed this is the key). Bits 1-7 are the pre-existing speaker/
        // haptic audio-enable flags. Gated on the bt_mic_enable config toggle.
        pkt[4] = (get_config().bt_mic_enable && usb_mic_stream_active) ? 0b11111111 : 0b11111110;
        const auto buf_len = get_config().audio_buffer_length;
        pkt[5] = buf_len;
        pkt[6] = buf_len;
        pkt[7] = buf_len;
        pkt[8] = buf_len; // 这 4 个字节的作用未知，调整没有效果
        pkt[9] = buf_len; // audio buffer length 只有调整这个字节生效。
        pkt[10] = packetCounter++;
        // SetStateData
        pkt[11] = 0x10 | 0 << 6 | 1 << 7;
        pkt[12] = 63;
        state_set(pkt + 13,63);
        // Haptics Audio Data
        pkt[76] = 0x12 | 0 << 6 | 1 << 7;
        pkt[77] = SAMPLE_SIZE;
        memcpy(pkt + 78, haptic_buf, SAMPLE_SIZE);
#if !DISABLE_SPEAKER_PROC
        // Speaker Audio Data
        // fixed65w/audio-route: use the host-selected DualSense AudioControl
        // output path when present. Fall back to 65v's physical jack route when
        // no host AudioControl was seen yet. This is what lets DualSense Tester
        // send one sine to the controller speaker and another to headphones
        // without inventing a second Windows audio device.
        pkt[142] = state_bt_audio_subreport(plug_headset) | 0 << 6 | 1 << 7;
        pkt[143] = 200;
        critical_section_enter_blocking(&opus_cs);
        memcpy(pkt + 144, opus_buf, 200);
        critical_section_exit(&opus_cs);
#endif

        bt_write(pkt, sizeof(pkt));
        g_bt_packets++;
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 24, 6);
 #if !DISABLE_SPEAKER_PROC
    queue_init(&audio_fifo, sizeof(audio_raw_element), 2);
    critical_section_init(&opus_cs);
    multicore_launch_core1_with_stack(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
#endif

    // Mic path: queue + decoder live on core0 (audio_loop), separate from
    // the core1 speaker encoder. Mic Opus is mono / 48 kHz / 10 ms frames.
    queue_init(&mic_fifo, sizeof(mic_element), MIC_DECODE_DEPTH);          // deeper: tolerate BT bursts
    queue_init(&mic_decode_fifo, sizeof(mic_decoded_element), MIC_DECODE_DEPTH); // decoded-PCM jitter buffer
    int dec_error = 0;
    mic_decoder = opus_decoder_create(48000, MIC_CHANNELS, &dec_error);
    if (dec_error != 0 || mic_decoder == nullptr) {
        printf("[Audio] OpusDecoder create failed (err=%d)\n", dec_error);
        mic_decoder = nullptr;  // ensure audio_loop's null-guard short-circuits
    }
}

static OpusEncoder *encoder;
static WDL_Resampler resampler_audio;

void __not_in_flash_func(core1_entry)() {
    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_AUDIO, &error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    resampler_audio.SetMode(true, 0, false);
    resampler_audio.SetRates(51200, 48000);
    resampler_audio.SetFeedMode(true);
    resampler_audio.Prealloc(2, 512, 480);

    while (true) {
        static audio_raw_element audio_element{};
        queue_remove_blocking(&audio_fifo, &audio_element);
        // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
        WDL_ResampleSample *in_buf;
        int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
        for (int i = 0; i < nframes * 2; i++) {
            in_buf[i] = audio_element.data[i];
        }
        static WDL_ResampleSample out_buf[480 * 2];
        resampler_audio.ResampleOut(out_buf, nframes, 480, 2);

        static uint8_t out[200];
        (void) opus_encode_float(encoder, out_buf, 480, out, 200);
        critical_section_enter_blocking(&opus_cs);
        memcpy(opus_buf, out, 200);
        critical_section_exit(&opus_cs);
    }
}
