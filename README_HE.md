## v0.2.11
תיקון יציבות MA2 Telnet: heartbeat שקט כל 25 שניות, בדיקת הצלחת שליחת פקודות, reconnect מהיר בכשל שליחה, ותצוגת `TELNET cmd` זמנית בלבד.

## v0.2.9

- נוסף אחוז סוללה של השלט בצד ימין למעלה במסך הראשי, כולל אייקון סוללה קטן.

## v0.2.8

- תיקון מיפוי D-Pad במצב עבודה רגיל: Right = `Next`, Left = `Previous`, Up = `Key Up`, Down = `Key Down`.
- מצב הגדרות נשאר כמו קודם: Mute דולק, D-Pad עורך, Triangle שומר.

## v0.2.4

- מבוסס על v0.2.3.
- במצב עבודה רגיל, כשה-Mute כבוי: D-Pad שולח `Next` / `Previous` ל-MA2 במקום `Key Up/Down/Left/Right`.
- Right/Down = `Next`, Left/Up = `Previous`.
- מצב עריכת הגדרות נשאר עם Mute דולק: D-Pad עורך, Triangle שומר.

## v0.2.3

- מבוסס על v0.2.1 העובד.
- נוספו Step נפרד לכל Zone בטווח 0.1-20.
- נוסף Rate נפרד לכל Zone במילישניות.
- כפתור Mute בשלט עושה Toggle למצב עריכת הגדרות. כשהאור הכתום דולק: D-Pad/Triangle עורכים ושומרים הגדרות. ב-v0.2.3 כשהאור כבוי D-Pad שלח Key Up/Down/Left/Right ל-MA2; ב-v0.2.4 זה הוחלף ל-Next/Previous.
- Right Stick נשאר Pan/Tilt בלבד.


## v0.2.1
- Build fix: added the missing public MA2 Telnet status and remote-control wrappers used by main/OLED.
- Right-stick report parser now calls the Telnet Pan/Tilt drive function from the exported ma2_remote_process_report() hook.



## v0.2.0 buildfix
- Fixed TinyUSB 0.20 network API: tud_network_xmit() is void, readiness is checked via tud_network_can_xmit().
- Fixed MAC address definition warning.
# DS5 MA2 Telnet USBNet

Firmware נקי ל-Pico 2 W: השלט DualSense מתחבר ל-Pico ב-Bluetooth, וה-Pico מזדהה למחשב כהתקן רשת USB ושולח פקודות Telnet ל-grandMA2.

אין כאן Gamepad HID, אין אודיו, אין MIDI, ואין Wi-Fi.

ראה הוראות מלאות:

```text
docs/MA2_TELNET_USBNET_HE.md
```


## 0.1.2 build fix
- TinyUSB 0.20 config updated: uses `CFG_TUD_ECM_RNDIS` instead of deprecated `CFG_TUD_NET`.
- Removed endpoint-size redefinition warning.
- Guards `CFG_TUSB_MCU` to avoid Pico SDK command-line redefinition.


### תוספות v0.2.6
סטיק שמאלי בציר Y שולט Dim. בתפריט יש Step/Rate לכל Zone וגם מיפוי כפתורים. Mute מדליק/מכבה מצב הגדרות; כשהוא כבוי, הכפתורים הממופים שולחים פקודות Telnet ל-MA2.
