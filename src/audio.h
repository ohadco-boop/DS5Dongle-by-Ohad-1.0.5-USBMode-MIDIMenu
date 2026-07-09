//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H

#include <cstdint>

void audio_init();
void audio_loop();
void core1_entry();
void set_headset(bool state);
void set_headset_state(uint8_t flags53, uint8_t flags54);
bool audio_headphones_plugged();
bool audio_headset_mic_plugged();
bool audio_external_mic_active();
uint8_t audio_jack_flags53();
uint8_t audio_jack_flags54();

// Accessors used by the optional OLED add-on (diag + VU meter screens).
uint32_t audio_fifo_drops();
uint32_t opus_fifo_drops();
uint8_t  audio_peak_speaker();   // 0..255, decays on read
uint8_t  audio_peak_haptic();    // 0..255, decays on read

// Byte-flow counters for the Diagnostics screen + web emulator.
uint32_t audio_usb_frames();
bool audio_usb_active();       // true while USB speaker or mic audio interface is effectively active

// USB Audio route recovery hooks. Called from TinyUSB SET_INTERFACE callbacks
// when the host opens/closes the speaker or microphone streaming interfaces.
// These are intentionally state-machine hooks, not diagnostics.
void audio_usb_speaker_interface_changed(bool active);
void audio_usb_microphone_interface_changed(bool active);

uint32_t audio_bt_packets();
uint32_t audio_mic_frames();   // count of mic Opus frames decoded + written
int32_t  audio_mic_last_decoded(); // last opus_decode return — neg = error, 480 = OK
uint16_t audio_mic_last_want();    // bytes asked of tud_audio_write
uint16_t audio_mic_last_wrote();   // bytes TinyUSB FIFO actually accepted
uint8_t  audio_mic_last_toc();     // first byte of last Opus packet (frame config)
uint32_t audio_mic_plc_frames();   // count of packet-loss-concealment frames generated

// Called from on_bt_data() in main.cpp when the DS5 sends a mic-tagged
// 0x31 input report. Buffer must point at MIC_OPUS_SIZE (71) bytes of
// Opus payload.
void mic_add_queue(const uint8_t *data);

#endif //DS5_BRIDGE_AUDIO_H