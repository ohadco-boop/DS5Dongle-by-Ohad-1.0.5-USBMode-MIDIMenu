## v0.2.12
- Telnet stability pass: uses real Telnet NOP keep-alive instead of empty command lines.
- Added connect/login timeouts so T:---- / TELNET retry cannot stay stuck until USB replug.
- Retains the v0.2.11 temporary TELNET cmd display fix.

## v0.2.11
MA2 Telnet stability fix: silent 25s heartbeat, checked command writes, fast reconnect on failed send, and temporary `TELNET cmd` OLED status.

## v0.2.9

- Added controller battery percent and a small battery icon on the top-right of the main OLED screen.

## v0.2.8

- Fixed live D-Pad mapping: Right = `Next`, Left = `Previous`, Up = `Key Up`, Down = `Key Down`.
- Settings mode remains behind Mute LED on: D-Pad edits, Triangle saves.

## v0.2.3

- Based on the working v0.2.1 branch.
- Added per-zone Pan/Tilt step, adjustable from 0.1 to 20.
- Added per-zone repeat rate in milliseconds.
- DualSense Mute toggles Pico settings mode. Orange mute LED on: D-Pad/Triangle edit and save settings. Mute LED off: D-Pad sends Key Up/Down/Left/Right to MA2.
- Right Stick remains dedicated to Pan/Tilt.


## v0.2.1
- Build fix: added the missing public MA2 Telnet status and remote-control wrappers used by main/OLED.
- Right-stick report parser now calls the Telnet Pan/Tilt drive function from the exported ma2_remote_process_report() hook.



## v0.2.0 buildfix
- Fixed TinyUSB 0.20 network API: tud_network_xmit() is void, readiness is checked via tud_network_can_xmit().
- Fixed MAC address definition warning.
# DS5 MA2 Telnet USBNet

Clean Pico 2 W firmware: DualSense connects to the Pico over Bluetooth; the Pico enumerates as a USB network adapter and sends Telnet commands to grandMA2/onPC.

No Gamepad HID, no USB Audio, no MIDI, no Wi-Fi.

See:

```text
docs/MA2_TELNET_USBNET_HE.md
```


## 0.1.2 build fix
- TinyUSB 0.20 config updated: uses `CFG_TUD_ECM_RNDIS` instead of deprecated `CFG_TUD_NET`.
- Removed endpoint-size redefinition warning.
- Guards `CFG_TUSB_MCU` to avoid Pico SDK command-line redefinition.


### v0.2.8 additions
Left stick Y controls Dim. Settings now include per-zone step/rate and a button mapping menu. Mute toggles Settings mode; when Mute is off, mapped controller buttons send MA2 Telnet commands.
