# User Manual - DS5Dongle by Ohad v1.0.5

## What is DS5Dongle by Ohad?
DS5Dongle by Ohad is a Raspberry Pi Pico 2 W based dongle that connects a DualSense controller to a computer over Bluetooth and exposes it over USB. The Waveshare Pico-OLED-1.3 display shows connection status, settings, diagnostics, audio status, button remapping, and pairing slot information.

Version 1.0.5 is the first stable release in the 1.0.x branch. The UF2 file name is `DS5Dongle-by-Ohad-1.0.5.uf2`.

## Quick start
1. Connect the Pico 2 W to the computer using USB.
2. Update firmware by dragging the UF2 file to the Pico BOOTSEL drive.
3. Wait for the dongle to show the Boot screen and then the Status screen.
4. Pair or reconnect the DualSense controller over Bluetooth.
5. Use `KEY0` and `KEY1` on the OLED board to switch screens.

## OLED controls
- `KEY0`: move to the next screen.
- `KEY1`: move to the previous screen.
- Long press `KEY1`: change OLED brightness.
- `KEY0 + KEY1` for about one second: reboot the Pico.
- Controller shortcut: `Options + D-Pad Right/Left` switches OLED screens without touching the dongle.

## OLED screens

### Boot
A short startup screen showing the product name and firmware version.

### Status
Shows firmware version, Bluetooth connection state, controller address, battery percentage, charge indication, sticks, triggers, D-Pad, and face buttons.

### Slots
Manages up to 4 controller pairing slots. The active slot is marked with `*`. Empty slots are shown as `(empty)`.

### Lightbar
Controls the DualSense lightbar mode. `HOST` lets the computer/game control the color. `BATT` lets the dongle show a battery-based color.

### Trigger Test
Tests the adaptive trigger feedback path for L2/R2.

### Gyro Tilt
Shows motion/tilt data for movement testing.

### Touchpad
Shows touchpad state and detected finger count.

### Diagnostics
Shows debug information such as uptime, USB audio rate, Bluetooth rate, HCI error count, and BT state.

### BT Signal
Shows Bluetooth RSSI in dBm and a signal quality indicator.

### VU Meters
Shows audio activity meters for speaker and haptics paths.


### Help
A built-in quick guide on the OLED. Scroll with `D-Pad Up/Down`. It explains screen navigation, menu navigation, saving, power-off, AudioKeep, and pairing. It is read-only and does not change settings.

### Settings
Firmware settings screen. Navigate with D-Pad, change values with Left/Right, and save with Triangle.

### Remap
Button remapping screen. Allows mapping a physical button to another button, disabling a button, or assigning it to `PicoMic`.

## Settings reference
- `Hap Gain`: base haptics gain.
- `Spk Vol`: speaker/audio volume.
- `Idle`: automatic disconnect timeout, or Off.
- `Pico LED`: enables/disables the Pico onboard LED.
- `Poll`: polling mode - 250Hz / 500Hz / RT.
- `AudBuf`: audio buffer size.
- `Ctrl`: controller mode - DS5 / DSE / Auto.
- `AutoHap`: audio-derived haptics mode.
- `AH Gain`: AutoHaptics gain.
- `AH LP`: AutoHaptics low-pass filter.
- `OLED Bright`: OLED brightness.
- `OLED Rot`: rotates the OLED display by 180 degrees.
- `Mic Gain`: Bluetooth microphone gain.
- `ScrDim`: time before the OLED dims.
- `ScrOff`: time before the OLED turns off.
- `BT Mic`: enables the controller microphone over Bluetooth.
- `CtrlWake`: controller activity wakes the OLED.
- `PowerCombo`: `PS + Options` safe controller disconnect/poweroff.
- `AudioKeep`: when ON, active audio prevents Idle shutdown.
- `Reset to defaults`: resets settings to defaults. Requires holding Triangle.
- `Wipe all slots`: clears all pairing slots. Requires holding Triangle.

## How to use Remap
1. Open the Remap screen.
2. Select a row using D-Pad Up/Down.
3. Change the target using D-Pad Left/Right.
4. Save with Triangle.
5. The `Remap on/off` row controls whether the mapping table is active. When it is off, mappings are not applied to the report sent to the computer.

Supported targets: `L2`, `L1`, `Share`, `Up`, `Left`, `Down`, `Right`, `L3`, `R2`, `R1`, `Opt`, `Tri`, `Cir`, `Cross`, `Sq`, `R3`, `PS`, `Pad`, `Mute`, `Off`, `PicoMic`.

Default behavior: most buttons pass through normally. The `Mute` button is mapped to `PicoMic` so it can locally control the dongle's Bluetooth microphone path.

## Safe poweroff
PowerCombo and Idle shutdown use a safe poweroff path. The dongle returns to the Status screen, shows `Powering Off...`, saves pending changes, and then disconnects the controller.

## AudioKeep
When AudioKeep is ON, Idle shutdown will not power off the controller while the computer is actively streaming audio through the dongle. This is useful for listening to music or using headphones through the controller without pressing buttons.

## Firmware update
1. Hold BOOTSEL on the Pico and connect USB.
2. A drive named `RPI-RP2` appears.
3. Drag `DS5Dongle-by-Ohad-1.0.5.uf2` onto the drive.
4. The Pico reboots into the new firmware.

## Quick troubleshooting
- Controller does not connect: switch slots or wipe slots, then pair again.
- No audio: verify that Windows selected the dongle as the audio device, and avoid extreme `AudBuf` values.
- Controller shuts down while music is playing: make sure AudioKeep is ON.
- REMAP does not work: make sure Remap is ON and Triangle save completed with `Saved`.
- OLED is off: press `KEY0` / `KEY1`, or check `ScrOff` / `ScrDim` in Settings.
