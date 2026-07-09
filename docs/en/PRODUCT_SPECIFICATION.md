# Product Specification - DS5Dongle by Ohad v1.0.4

## Product identification
- Product name: DS5Dongle by Ohad
- Firmware version: 1.0.4
- UF2 file name: `DS5Dongle-by-Ohad-1.0.4.uf2`
- Target hardware: Raspberry Pi Pico 2 W + Waveshare Pico-OLED-1.3

## Hardware
- MCU: RP2350 Dual Cortex-M33
- Target board: Pico 2 W
- Bluetooth/Wi-Fi chip: CYW43
- OLED: 128x64 SH1107 monochrome display
- OLED interface: SPI
- Display buttons: KEY0 / KEY1

## USB
- USB Device stack through TinyUSB
- HID bridge for DualSense reports
- USB Audio Class support
- Sample rate: 48 kHz
- Resolution: 16-bit
- Speaker path: USB host -> Pico -> Bluetooth -> DualSense
- Microphone path: DualSense BT mic -> Pico -> USB capture endpoint

## Bluetooth
- Bluetooth Classic HID over L2CAP
- Multi-slot pairing support: 4 slots
- Pairing slots are stored in Flash
- RSSI measurement for the BT Signal screen

## OLED UI
Included OLED screens:
1. Boot
2. Status
3. Slots
4. Lightbar
5. Trigger Test
6. Gyro Tilt
7. Touchpad
8. Diagnostics
9. BT Signal
10. VU Meters
11. Settings
12. Remap

## Supported settings
- Haptic Gain
- Speaker Volume
- Idle Timeout
- Pico LED
- Polling Rate
- Audio Buffer
- Controller Mode
- AutoHaptics
- AutoHaptics Gain
- AutoHaptics Low Pass
- OLED Brightness
- OLED Rotation
- Mic Gain
- Screen Dim Timeout
- Screen Off Timeout
- BT Mic Enable
- Controller Wake Display
- PowerCombo
- AudioKeep
- Reset Defaults
- Wipe Slots

## Remap
- Source inputs: 19 buttons
- Standard targets: 19 DualSense buttons
- Special targets: `Off`, `PicoMic`
- Remap is applied only to the report sent to the computer.
- Internal OLED navigation, PowerCombo, and system shortcuts remain raw so they continue to work even if the user changes button mapping.

## Power management
- Idle disconnect according to user setting.
- AudioKeep prevents Idle disconnect while USB Audio is active.
- PowerCombo: `PS + Options` within a short window triggers safe controller disconnect/poweroff.
- Before disconnect, the firmware performs a safe-save flow for settings, lightbar, remap, and pending Flash writes.
- AudioFix behavior: before a forced save/disconnect, the firmware sends an audio-safe mute/off state and clears cached host audio routing after local disconnect.

## Build / release
- GitHub Actions builds the UF2 automatically.
- `FIRMWARE_VERSION` in `.github/workflows/build.yml` is set to `1.0.4`.
- CMake passes the version to the firmware as `FIRMWARE_VERSION` and `FIRMWARE_VERSION_SHORT`.

## Known limits
- The ZIP package does not include a prebuilt UF2. Build is intended through GitHub Actions or a local environment with Pico SDK, TinyUSB 0.20.0, Opus, and WDL.
- Audio and Bluetooth quality depend on RF conditions, host computer behavior, and controller state.
