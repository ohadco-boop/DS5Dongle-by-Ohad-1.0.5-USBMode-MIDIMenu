# מפרט מוצר - DS5Dongle by Ohad v1.0.4

## זיהוי מוצר
- שם: DS5Dongle by Ohad
- גרסה: 1.0.4
- שם UF2: `DS5Dongle-by-Ohad-1.0.4.uf2`
- יעד חומרה: Raspberry Pi Pico 2 W + Waveshare Pico-OLED-1.3

## חומרה
- MCU: RP2350 Dual Cortex-M33
- לוח יעד: Pico 2 W
- Bluetooth/Wi-Fi chip: CYW43
- OLED: 128x64 SH1107, מונוכרום
- ממשק OLED: SPI
- כפתורים על התצוגה: KEY0 / KEY1

## USB
- USB Device דרך TinyUSB
- HID bridge לשלט DualSense
- USB Audio Class
- Sample rate: 48 kHz
- Resolution: 16-bit
- Speaker path: USB host -> Pico -> Bluetooth -> DualSense
- Microphone path: DualSense BT mic -> Pico -> USB capture endpoint

## Bluetooth
- Bluetooth Classic HID over L2CAP
- תמיכה בצימוד Multi-slot: 4 סלוטים
- שמירת סלוטים בפלאש
- מדידת RSSI למסך BT Signal

## OLED UI
מסכי OLED כלולים:
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

## Settings נתמכים
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
- מספר מקורות: 19 כפתורים
- יעדים רגילים: 19 כפתורי DualSense
- יעדים מיוחדים: Off, PicoMic
- Remap מוחל על הדוח שיוצא למחשב בלבד.
- קלט פנימי של OLED, PowerCombo וקיצורי מערכת נשאר raw כדי שלא יישבר גם אם המשתמש שינה מיפוי.

## ניהול חשמל
- Idle disconnect לפי הגדרה.
- AudioKeep מונע Idle disconnect בזמן שיש USB Audio פעיל.
- PowerCombo: PS + Options בתוך חלון קצר גורם לכיבוי/ניתוק בטוח.
- לפני ניתוק מתבצע safe-save להגדרות, Lightbar, Remap וכתיבות Flash ממתינות.

## Build / Release
- GitHub Actions בונה UF2 אוטומטי.
- `FIRMWARE_VERSION` בקובץ `.github/workflows/build.yml` מוגדר ל-1.0.4.
- CMake מעביר את VERSION לקוד כ-`FIRMWARE_VERSION` ו-`FIRMWARE_VERSION_SHORT`.

## גבולות ידועים
- אין בנייה מקומית של UF2 בחבילת ZIP הזו; הבנייה מיועדת ל-GitHub Actions או לסביבה מקומית עם Pico SDK, TinyUSB 0.20.0, Opus ו-WDL.
- איכות שמע/BT תלויה בסביבת RF, במחשב, ובמצב השלט.
