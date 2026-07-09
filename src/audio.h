#pragma once
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
uint32_t audio_fifo_drops();
uint32_t opus_fifo_drops();
uint8_t  audio_peak_speaker();
uint8_t  audio_peak_haptic();
uint32_t audio_usb_frames();
bool audio_usb_active();
void audio_usb_speaker_interface_changed(bool active);
void audio_usb_microphone_interface_changed(bool active);
uint32_t audio_bt_packets();
uint32_t audio_mic_frames();
int32_t  audio_mic_last_decoded();
uint16_t audio_mic_last_want();
uint16_t audio_mic_last_wrote();
uint8_t  audio_mic_last_toc();
uint32_t audio_mic_plc_frames();
void mic_add_queue(const uint8_t *data);
