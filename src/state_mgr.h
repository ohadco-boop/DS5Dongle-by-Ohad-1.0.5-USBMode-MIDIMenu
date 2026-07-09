//
// Created by awalol on 2026/5/15.
//

#ifndef DS5_BRIDGE_STATE_MGR_H
#define DS5_BRIDGE_STATE_MGR_H

#include <cstdint>

void state_init();
void state_set(uint8_t *data, const uint8_t size);
void state_set_external_mic_route(bool enabled);
void state_clear_host_audio_route();
// Ohad fixed43: controller Mute button LED mirrors Pico BT-mic setting.
// muted=true => orange mute LED ON (Pico mic off), muted=false => LED OFF.
void state_set_mute_light(bool muted);
void state_update(const uint8_t *data, const uint8_t size);

// Host-controlled DualSense audio routing helper. Returns the Bluetooth audio
// sub-report selector for the compressed speaker/headset payload:
//   0x13 = controller speaker, 0x14 = mono headset,
//   0x15 = headset+speaker split, 0x16 = headset stereo.
// If the host has not sent AudioControl yet, it falls back to the physical
// jack state so 65v speaker/headset behavior stays unchanged.
uint8_t state_bt_audio_subreport(bool physical_headset_plugged);

// Lightbar RGB lives in the persistent state[] block (SetStateData LedRed/
// Green/Blue) that gets stamped into every outbound BT packet. The OLED
// lightbar service writes it directly so a firmware-chosen color rides every
// host/audio frame instead of only the transient send_lightbar_color() packet.
void state_set_led(uint8_t r, uint8_t g, uint8_t b);
void state_get_led(uint8_t *r, uint8_t *g, uint8_t *b);

#endif //DS5_BRIDGE_STATE_MGR_H
