# DS5 MA2 Telnet USBNet

Firmware נקי ל-Pico 2 W: השלט DualSense מתחבר ל-Pico ב-Bluetooth, וה-Pico מזדהה למחשב כהתקן רשת USB ושולח פקודות Telnet ל-grandMA2.

אין כאן Gamepad HID, אין אודיו, אין MIDI, ואין Wi-Fi.

ראה הוראות מלאות:

```text
docs/MA2_TELNET_USBNET_HE.md
```


## 0.1.1 build fix
- TinyUSB 0.20 config updated: uses `CFG_TUD_ECM_RNDIS` instead of deprecated `CFG_TUD_NET`.
- Removed endpoint-size redefinition warning.
- Guards `CFG_TUSB_MCU` to avoid Pico SDK command-line redefinition.
