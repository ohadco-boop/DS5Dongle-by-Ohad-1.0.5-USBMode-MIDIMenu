#ifndef DS5_BRIDGE_MIDI_PT_H
#define DS5_BRIDGE_MIDI_PT_H

#include <cstdint>

// MIDI-only Pan/Tilt bridge for MA2.
// Feed it the 63-byte DualSense input report (data+3 from BT 0x31 frames),
// then call midi_pt_loop() regularly from the main loop.
void midi_pt_init();
void midi_pt_note_report(const uint8_t *report, uint16_t len);
void midi_pt_loop();

#endif // DS5_BRIDGE_MIDI_PT_H
