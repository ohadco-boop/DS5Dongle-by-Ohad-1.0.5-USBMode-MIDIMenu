//
// Created by awalol on 2026/5/15.
//

#include <cstddef>
#include <cstring>

#include "utils.h"
#include "state_mgr.h"
#include "config.h"
#include "oled.h"

// Set by the OLED lightbar service (src/oled.cpp). While true, the firmware
// owns the lightbar (an OLED mode or the charging pulse) and the host's
// AllowLedColor writes are suppressed below so they can't stomp it.
extern bool g_lightbar_override;

namespace {
    constexpr size_t kAudioControlOffset = offsetof(SetStateData, MuteLightMode) - sizeof(uint8_t);
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kMotorPowerLevelOffset = offsetof(SetStateData, HostTimestamp) + sizeof(uint32_t);
    constexpr size_t kAudioControl2Offset = kMotorPowerLevelOffset + sizeof(uint8_t);
    constexpr size_t kHapticLowPassFilterOffset = offsetof(SetStateData, LightFadeAnimation) - 2 * sizeof(uint8_t);
    constexpr size_t kPlayerIndicatorsOffset = offsetof(SetStateData, LedRed) - sizeof(uint8_t);

    constexpr uint8_t kFlagAllowHeadphoneVolume = 0x10;
    constexpr uint8_t kFlagAllowSpeakerVolume   = 0x20;
    constexpr uint8_t kFlagAllowMicVolume       = 0x40;
    constexpr uint8_t kFlagAllowAudioControl    = 0x80;
    constexpr uint8_t kFlagAllowAudioMute       = 0x02;
    constexpr uint8_t kFlagAllowAudioControl2   = 0x80;

    constexpr uint8_t kMuteSpeaker              = 0x20;
    constexpr uint8_t kMuteHeadphones           = 0x40;

    // Mapping matches the DualSense BT-audio sub-reports used in audio.cpp:
    // OutputPathSelect 0 = L/R headset, 1 = mono headset,
    //                  2 = headset + speaker split, 3 = speaker only.
    constexpr uint8_t kBtSubreportSpeaker       = 0x13;
    constexpr uint8_t kBtSubreportHeadsetMono   = 0x14;
    constexpr uint8_t kBtSubreportSplit         = 0x15;
    constexpr uint8_t kBtSubreportHeadsetStereo = 0x16;

    static uint8_t output_path_select_from_audio_control(uint8_t audio_control) {
        return (uint8_t)((audio_control >> 4) & 0x03);
    }

    static uint8_t set_output_path_select(uint8_t audio_control, uint8_t path) {
        return (uint8_t)((audio_control & (uint8_t)~0x30) | ((path & 0x03) << 4));
    }
}

static constexpr uint8_t state_init_data[63] = {
    0xfd, 0xf7, 0x0, 0x0,
    0x7f, 0x64, // Headphones, Speaker
    0x40, 0x9, 0x0, 0x00, 0x0, 0x0, 0x0, 0x0, // VolumeMic=64, MuteControl all clear (no PowerSave)
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
    0x7, 0x0, 0x0, 0x2, 0x1,
    0x00,
    0xff, 0xd7, 0x00 // RGB LED: R, G, B (Nijika Color!)✨
};

uint8_t state[63]{};
static volatile bool g_external_mic_route = false;

// fixed65w/audio-route: 65v deliberately forced AudioControl from jack state
// and ignored host AudioControl writes. That made DualSense Tester unable to
// select speaker vs headset independently: both sine buttons followed the same
// physical-jack route. Keep the 65v fallback, but once the host explicitly sends
// AudioControl, mirror the host-selected route/volume/mute into the BT packets.
static volatile bool g_host_audio_control_seen = false;
static volatile bool g_host_audio_control2_seen = false;
static volatile bool g_host_audio_mute_seen = false;
static volatile uint8_t g_host_audio_control = 0x30;   // default speaker/internal route
static volatile uint8_t g_host_audio_control2 = 0x02;
static volatile uint8_t g_host_mute_control = 0x00;
static volatile uint8_t g_host_headphone_volume = 0x7f;
static volatile uint8_t g_host_speaker_volume = 0x64;
static volatile uint8_t g_host_mic_volume = 0x40;

void state_clear_host_audio_route() {
    g_host_audio_control_seen = false;
    g_host_audio_control2_seen = false;
    g_host_audio_mute_seen = false;
    g_host_audio_control = 0x30;
    g_host_audio_control2 = 0x02;
    g_host_mute_control = 0x00;
    g_host_headphone_volume = 0x7f;
    g_host_speaker_volume = 0x64;
    g_host_mic_volume = 0x40;
}

void state_set_external_mic_route(bool enabled) {
    // A real DualSense changes the physical jack route without re-enumerating.
    // If the jack is actually plugged/unplugged, drop any old host route so the
    // default follows the new physical state until the host sends AudioControl
    // again. Do not reset on MIC/E flag flaps while the aggregate route remains
    // the same, because those can happen as a result of AudioControl itself.
    if (g_external_mic_route != enabled) {
        g_host_audio_control_seen = false;
        g_host_audio_control2_seen = false;
        g_host_audio_mute_seen = false;
    }
    g_external_mic_route = enabled;
}

static bool state_should_redirect_default_speaker_to_headset(bool physical_headset_plugged) {
    if (!physical_headset_plugged || !g_host_audio_control_seen) return false;

    const uint8_t path = output_path_select_from_audio_control(g_host_audio_control);
    if (path != 3) return false;  // only the normal speaker/internal endpoint needs jack redirect

    const uint8_t mute = g_host_audio_mute_seen ? g_host_mute_control : 0x00;
    const bool headphones_muted = ((mute & kMuteHeadphones) != 0) || g_host_headphone_volume == 0x00;

    // Warzone/Spider-Man style games often keep sending the default speaker path
    // (0x30) after the user plugs headphones into the controller. A real wired
    // DualSense routes that normal endpoint to the jack, not to a now-muted
    // internal speaker. Still respect explicit tester-style speaker selection
    // when the host mutes/zeroes headphones.
    return !headphones_muted;
}

uint8_t state_bt_audio_subreport(bool physical_headset_plugged) {
    if (!g_host_audio_control_seen) {
        return physical_headset_plugged ? kBtSubreportHeadsetStereo : kBtSubreportSpeaker;
    }

    const uint8_t mute = g_host_audio_mute_seen ? g_host_mute_control : 0x00;
    const bool speaker_muted = (mute & kMuteSpeaker) != 0 || g_host_speaker_volume == 0x00;
    const bool headphones_muted = (mute & kMuteHeadphones) != 0 || g_host_headphone_volume == 0x00;
    const uint8_t path = output_path_select_from_audio_control(g_host_audio_control);

    // If the host mutes one side, prefer the audible side even if OutputPathSelect
    // was left at a mixed/default value. This is how tester-style speaker/headset
    // toggles often express routing.
    if (speaker_muted && !headphones_muted) return kBtSubreportHeadsetStereo;
    if (headphones_muted && !speaker_muted) return kBtSubreportSpeaker;

    if (state_should_redirect_default_speaker_to_headset(physical_headset_plugged)) {
        return kBtSubreportHeadsetStereo;
    }

    switch (path) {
        case 0: return kBtSubreportHeadsetStereo;
        case 1: return kBtSubreportHeadsetMono;
        case 2: return kBtSubreportSplit;
        case 3:
        default: return kBtSubreportSpeaker;
    }
}

void state_set_mute_light(bool muted) {
    // Byte 1 bit0 = AllowMuteLight, byte 8 = MuteLightMode in SetStateData.
    state[1] |= 0x01;
    state[offsetof(SetStateData, MuteLightMode)] = muted ? MuteLight::On : MuteLight::Off;
}

static void state_apply_pico_mic_mute_light(uint8_t *data, const uint8_t size) {
    if (size <= offsetof(SetStateData, MuteLightMode)) return;
    // The Pico owns the mute LED: ON means the Pico BT mic path is disabled.
    data[1] |= 0x01;
    data[offsetof(SetStateData, MuteLightMode)] = get_config().bt_mic_enable ? MuteLight::Off : MuteLight::On;
}

void state_init() {
    memcpy(state, state_init_data, sizeof(state));
    state_set_mute_light(!get_config().bt_mic_enable);
}

void state_set(uint8_t *data, const uint8_t size) {
    if (size > 63) {
        printf("[StateMgr] Warning: State Set over 63 bytes\n");
    }
    memcpy(data, state, size);

    // fixed65w/audio-route:
    // - Default/fallback remains the 65v jack-based reversible route.
    // - If the host has explicitly sent AudioControl, preserve the host's
    //   speaker/headset route so DualSense Tester can target them independently.
    if (size > 37) {
        if (g_host_audio_control_seen) {
            const bool redirect_default_speaker = state_should_redirect_default_speaker_to_headset(g_external_mic_route);

            data[0] |= (uint8_t)(kFlagAllowHeadphoneVolume | kFlagAllowAudioControl);
            data[4] = g_host_headphone_volume;

            if (redirect_default_speaker) {
                // Jack plugged + game keeps sending the default speaker path: mimic
                // wired DualSense behavior by routing the normal endpoint to the
                // 3.5mm headphones. Do not allow the stale speaker volume/control2
                // to pull the DS5 back to the internal speaker after ~0.5s.
                data[0] = (uint8_t)(data[0] & (uint8_t)~kFlagAllowSpeakerVolume);
                data[5] = 0x00;
                data[7] = set_output_path_select(g_host_audio_control, 0); // headphones L/R
                data[1] = (uint8_t)(data[1] & (uint8_t)~kFlagAllowAudioControl2);
                data[37] = 0x00;
            } else {
                data[0] |= kFlagAllowSpeakerVolume;
                data[5] = g_host_speaker_volume;
                data[7] = g_host_audio_control;
            }

            if (g_host_audio_mute_seen) {
                data[1] |= kFlagAllowAudioMute;
                data[9] = g_host_mute_control;
            }

            const uint8_t host_path = output_path_select_from_audio_control(data[7]);
            const bool speaker_path = host_path == 2 || host_path == 3;
            const bool speaker_muted = g_host_audio_mute_seen && ((g_host_mute_control & kMuteSpeaker) != 0);
            if (!redirect_default_speaker && g_host_audio_control2_seen) {
                data[1] |= kFlagAllowAudioControl2;
                data[37] = g_host_audio_control2;
            } else if (!redirect_default_speaker && speaker_path && !speaker_muted && g_host_speaker_volume != 0x00) {
                data[1] |= kFlagAllowAudioControl2;
                data[37] = 0x02;
            } else {
                data[1] = (uint8_t)(data[1] & (uint8_t)~kFlagAllowAudioControl2);
                data[37] = 0x00;
            }
        } else if (g_external_mic_route) {
            data[0] = (uint8_t)((data[0] | kFlagAllowAudioControl) &
                                (uint8_t)~kFlagAllowSpeakerVolume);
            data[1] = (uint8_t)(data[1] & (uint8_t)~kFlagAllowAudioControl2);
            data[4] = 0x7f; // headphones volume
            data[5] = 0x00; // speaker volume off
            data[7] = 0x00; // output path headphones
            data[37] = 0x00;
        } else {
            data[0] |= (uint8_t)(kFlagAllowAudioControl | kFlagAllowSpeakerVolume);
            data[1] |= kFlagAllowAudioControl2;
            data[4] = 0x7f;   // headphones volume stays max
            data[5] = 0x64;   // speaker volume
            data[7] = 0x30;   // output path speaker / internal route
            data[37] = 0x02;  // speaker preamp gain
        }
        // Do not set AllowMicVolume here; host mic gain is handled in USB PCM scaling.
    }

    state_apply_pico_mic_mute_light(data, size);
}

void state_set_led(uint8_t r, uint8_t g, uint8_t b) {
    state[offsetof(SetStateData, LedRed) + 0] = r;
    state[offsetof(SetStateData, LedRed) + 1] = g;
    state[offsetof(SetStateData, LedRed) + 2] = b;
}

void state_get_led(uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = state[offsetof(SetStateData, LedRed) + 0];
    *g = state[offsetof(SetStateData, LedRed) + 1];
    *b = state[offsetof(SetStateData, LedRed) + 2];
}

void state_update(const uint8_t *data, const uint8_t size) {
    if (size < sizeof(SetStateData)) {
        printf(
            "[StateMgr] Error: SetStateData at least %u bytes\n",
            static_cast<unsigned>(sizeof(SetStateData))
        );
        return;
    }

    SetStateData update{};
    memcpy(&update, data, sizeof(update));

    const auto copy_if_allowed = [&](const bool allowed, const size_t offset, const size_t length) {
        if (allowed) {
            memcpy(state + offset, data + offset, length);
        }
    };
    auto set_bit = [](uint8_t &byte, const int bit, const bool value) {
        byte = (byte & ~(1 << bit)) | (value << bit);
    };

    set_bit(state[0], 0, update.EnableRumbleEmulation);
    set_bit(state[0], 1, update.UseRumbleNotHaptics);
    set_bit(state[38], 2, update.EnableImprovedRumbleEmulation);
    copy_if_allowed(
        update.UseRumbleNotHaptics || update.EnableRumbleEmulation,
        offsetof(SetStateData, RumbleEmulationRight),
        2
    );

    if (data[0] & kFlagAllowHeadphoneVolume) {
        copy_if_allowed(true, offsetof(SetStateData, VolumeHeadphones), sizeof(update.VolumeHeadphones));
        g_host_headphone_volume = data[offsetof(SetStateData, VolumeHeadphones)];
    }
    if (data[0] & kFlagAllowSpeakerVolume) {
        copy_if_allowed(true, offsetof(SetStateData, VolumeSpeaker), sizeof(update.VolumeSpeaker));
        g_host_speaker_volume = data[offsetof(SetStateData, VolumeSpeaker)];
    }
    if (data[0] & kFlagAllowMicVolume) {
        copy_if_allowed(true, offsetof(SetStateData, VolumeMic), sizeof(update.VolumeMic));
        g_host_mic_volume = data[offsetof(SetStateData, VolumeMic)];
    }
    if (data[0] & kFlagAllowAudioControl) {
        copy_if_allowed(true, kAudioControlOffset, sizeof(uint8_t));
        g_host_audio_control = data[kAudioControlOffset];
        g_host_audio_control_seen = true;
    }

    copy_if_allowed(
        update.AllowMuteLight,
        offsetof(SetStateData, MuteLightMode),
        sizeof(update.MuteLightMode)
    );

    if (data[1] & kFlagAllowAudioMute) {
        copy_if_allowed(true, kMuteControlOffset, sizeof(uint8_t));
        g_host_mute_control = data[kMuteControlOffset];
        g_host_audio_mute_seen = true;
    }

    copy_if_allowed(
        update.AllowRightTriggerFFB,
        offsetof(SetStateData, RightTriggerFFB),
        sizeof(update.RightTriggerFFB)
    );
    copy_if_allowed(
        update.AllowLeftTriggerFFB,
        offsetof(SetStateData, LeftTriggerFFB),
        sizeof(update.LeftTriggerFFB)
    );

    /*copy_if_allowed(
        update.AllowMotorPowerLevel,
        kMotorPowerLevelOffset,
        sizeof(uint8_t)
    );*/
    if (data[1] & kFlagAllowAudioControl2) {
        copy_if_allowed(true, kAudioControl2Offset, sizeof(uint8_t));
        g_host_audio_control2 = data[kAudioControl2Offset];
        g_host_audio_control2_seen = true;
    }
    /*copy_if_allowed(
        update.AllowHapticLowPassFilter,
        kHapticLowPassFilterOffset,
        sizeof(uint8_t)
    );*/

    copy_if_allowed(
        update.AllowColorLightFadeAnimation,
        offsetof(SetStateData, LightFadeAnimation),
        sizeof(update.LightFadeAnimation)
    );
    copy_if_allowed(
        update.AllowLightBrightnessChange,
        offsetof(SetStateData, LightBrightness),
        sizeof(update.LightBrightness)
    );
    copy_if_allowed(
        update.AllowPlayerIndicators,
        kPlayerIndicatorsOffset,
        sizeof(uint8_t)
    );
    copy_if_allowed(
        update.AllowLedColor && !g_lightbar_override && oled_lightbar_host_mode(),
        offsetof(SetStateData, LedRed),
        sizeof(update.LedRed) * 3
    );
}
