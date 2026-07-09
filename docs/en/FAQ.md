# FAQ - DS5Dongle by Ohad v1.0.4

## Does the ZIP include a UF2 file?
No. The UF2 is produced by GitHub Actions. The expected artifact name is `DS5Dongle-by-Ohad-1.0.4.uf2`.

## Which controller is supported?
The project is designed around the Sony DualSense controller.

## What does AudioKeep do?
When AudioKeep is ON, Idle shutdown is blocked while the computer is actively streaming audio through the dongle.

## Why does the Mute button default to PicoMic?
This lets the controller Mute button control the dongle's local Bluetooth microphone path instead of only passing through as a normal button.

## Can I change button mappings?
Yes. Use the Remap screen, change targets with D-Pad Left/Right, and save with Triangle.

## How do I safely disconnect the controller?
Use PowerCombo: `PS + Options`. The firmware returns to the Status screen, saves pending changes, and disconnects safely.
