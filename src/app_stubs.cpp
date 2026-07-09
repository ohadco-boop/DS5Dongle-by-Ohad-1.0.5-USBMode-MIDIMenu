#include <cstdint>
#include <cstdio>

// This firmware only needs DualSense BT input. Audio, gamepad HID, local power-off
// and diagnostics from the full DS5Dongle build are intentionally stubbed out.

void audio_init() {}
void audio_loop() {}
void core1_entry() {}
void set_headset(bool) {}
void set_headset_state(uint8_t, uint8_t) {}
bool audio_headphones_plugged() { return false; }
bool audio_headset_mic_plugged() { return false; }
bool audio_external_mic_active() { return false; }
uint8_t audio_jack_flags53() { return 0; }
uint8_t audio_jack_flags54() { return 0; }
uint32_t audio_fifo_drops() { return 0; }
uint32_t opus_fifo_drops() { return 0; }
uint8_t  audio_peak_speaker() { return 0; }
uint8_t  audio_peak_haptic() { return 0; }
uint32_t audio_usb_frames() { return 0; }
bool audio_usb_active() { return false; }
void audio_usb_speaker_interface_changed(bool) {}
void audio_usb_microphone_interface_changed(bool) {}
uint32_t audio_bt_packets() { return 0; }
uint32_t audio_mic_frames() { return 0; }
int32_t  audio_mic_last_decoded() { return 0; }
uint16_t audio_mic_last_want() { return 0; }
uint16_t audio_mic_last_wrote() { return 0; }
uint8_t  audio_mic_last_toc() { return 0; }
uint32_t audio_mic_plc_frames() { return 0; }
void mic_add_queue(const uint8_t*) {}

void controller_poweroff_request() {}
bool controller_poweroff_is_pending() { return false; }
void controller_poweroff_note_bt_disconnected() {}
