//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

// Ohad fixed63: Windows/UAC1 volume sync is always enabled, with no OLED
// Settings toggle. Speaker volume attenuates only the audio output path;
// microphone level attenuates the Pico BT mic before sending it to the host.
float usb_speaker_volume_gain();
float usb_mic_level_scale();

#endif //DS5_BRIDGE_USB_H