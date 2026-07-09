# DS5Dongle MIDI Only - grandMA2 mapping

This build makes the Pico enumerate as **USB MIDI only** (`DS5Dongle MIDI Only`).
It does not expose a gamepad HID interface to Windows in this firmware profile.

## Right stick Pan/Tilt notes

Set grandMA2 MIDI Remote rows as `Type = CMD`, Channel 1, and use these commands:

| Note | Command |
|---:|---|
| 60 | `Attribute "Pan" At - 1 If Selection` |
| 61 | `Attribute "Pan" At + 1 If Selection` |
| 62 | `Attribute "Tilt" At + 1 If Selection` |
| 63 | `Attribute "Tilt" At - 1 If Selection` |
| 64 | `Attribute "Pan" At - 3 If Selection` |
| 65 | `Attribute "Pan" At + 3 If Selection` |
| 66 | `Attribute "Tilt" At + 3 If Selection` |
| 67 | `Attribute "Tilt" At - 3 If Selection` |
| 68 | `Attribute "Pan" At - 10 If Selection` |
| 69 | `Attribute "Pan" At + 10 If Selection` |
| 70 | `Attribute "Tilt" At + 10 If Selection` |
| 71 | `Attribute "Tilt" At - 10 If Selection` |

The firmware automatically chooses the speed by stick distance:

- ~5%-37% = Speed 1 / notes 60-63
- ~37%-72% = Speed 2 / notes 64-67
- ~72%-100% = Speed 3 / notes 68-71

The hysteresis keeps the stick from jittering between speed bands near the boundary.

## Optional button notes

| Control | Note |
|---|---:|
| Cross | 80 |
| Circle | 81 |
| Square | 82 |
| Triangle | 83 |
| L1 | 84 |
| R1 | 85 |
| L2 | 86 |
| R2 | 87 |
| D-pad Up | 88 |
| D-pad Down | 89 |
| D-pad Left | 90 |
| D-pad Right | 91 |
| Create | 92 |
| Options | 93 |
| Touchpad | 94 |
| PS | 95 |

## Runtime USB Mode from OLED Settings

This version uses one UF2 only. The mode is selected on the OLED:

`Settings -> USB -> Gamepad / MIDI`

After changing the value, press Triangle to save. The Pico writes the setting to flash and reboots so TinyUSB can enumerate as the selected USB device.

- `Gamepad`: normal DualSense-compatible HID + USB Audio.
- `MIDI`: USB MIDI only. No Gamepad HID and no USB Audio interfaces are exposed to the host.

