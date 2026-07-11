

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
