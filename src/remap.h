//
// Button remapping — a 19-entry table persisted in its own flash sector,
// applied to the OUTGOING host HID report only (never the raw interrupt_in_data
// the OLED / reboot-combo logic reads). Edited from the web config tool over the
// already-declared 0xF6/0xF7 vendor reports. Apply logic + button set ported
// from SundayMoments/DS5_Bridge (credit).
//

#ifndef DS5_BRIDGE_REMAP_H
#define DS5_BRIDGE_REMAP_H

#include <cstdint>

// Number of remappable buttons (see RemapButton in remap.cpp). The table maps
// source index -> target index; 0xFF means "disabled" (source does nothing).
constexpr int kRemapCount = 19;

// Special remap targets that are not normal host buttons.
constexpr uint8_t kRemapTargetPicoMic = 0xFE; // local action: toggle Pico BT Mic
constexpr uint8_t kRemapTargetOff     = 0xFF; // disabled / source does nothing

// Wire-protocol version for the remap get/set framing carried over the existing
// 0xF6/0xF7 vendor reports (see cmd.cpp). Bump only on incompatible layout
// changes so the web tool can refuse a mismatched firmware.
constexpr uint8_t kRemapProtoVer = 3;

// Load the table from its dedicated flash sector. Call once at boot, after
// config_load(). A fresh/invalid sector defaults to identity (no remap).
void remap_load();

// Remap a 63-byte DS5 input-report COPY in place (buttons live in report[4,5,7,8,9]).
// No-op fast path when the table is identity. Must only ever touch the outgoing
// host report, not the raw interrupt_in_data.
void remap_apply(uint8_t *report);

// Validate + store + persist a new remap table (each entry < kRemapCount,
// kRemapTargetPicoMic, or kRemapTargetOff). Bumps the revision on success.
// Returns false if the table is invalid.
bool remap_set(const uint8_t *table);

// Copy the current remap table out.
void remap_get(uint8_t out[kRemapCount]);

// True when the current physical input report presses a source button mapped to
// the local Pico Mic toggle action. The physical Mute button action remains
// active even when Remap is globally off, so it behaves as a dedicated local
// mic key by default. Other sources mapped to PicoMic require Remap to be on.
bool remap_pico_mic_pressed(const uint8_t *report);

// Monotonic counter bumped on each successful remap_set — the web polls it to
// confirm a write landed. Runtime only (not persisted).
uint16_t remap_revision();

#endif // DS5_BRIDGE_REMAP_H
