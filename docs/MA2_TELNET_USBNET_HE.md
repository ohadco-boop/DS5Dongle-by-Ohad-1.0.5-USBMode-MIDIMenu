# DS5 MA2 Telnet USBNet - Pico 2 W

גרסה נפרדת ונקייה: אין Gamepad HID, אין USB Audio, אין MIDI, אין Wi-Fi.

השרשרת:

```text
DualSense --Bluetooth--> Pico 2 W --USB Network/RNDIS--> PC / grandMA2 onPC --Telnet--> MA2 Command Line
```

## ברירת מחדל

- Pico USB IP: `192.168.7.2`
- PC/MA2 USB-side IP: `192.168.7.1`
- MA2 Telnet port: `30000`
- Username: `administrator`
- Password: ריק
- Deadzone: `5%`
- Speed 1/2 boundary: `35%`
- Speed 2/3 boundary: `70%`

ב-Windows צריך להגדיר ידנית את מתאם הרשת החדש שנוצר מה-Pico:

```text
IP address: 192.168.7.1
Subnet mask: 255.255.255.0
Gateway: empty
DNS: empty
```

ב-grandMA2:

```text
Setup -> Console -> Global Settings -> Telnet -> Login Enabled
Port 30000
```

## שליטה ב-Pan/Tilt

סטיק ימין:

```text
Right Stick X left/right = Pan -/+
Right Stick Y up/down    = Tilt +/-
```

ה-Pico שולח פקודות טקסט ישירות ל-Telnet:

```text
Attribute "Pan" At - 1 If Selection
Attribute "Pan" At + 3 If Selection
Attribute "Tilt" At + 10 If Selection
```

המהירות נקבעת לפי כמה רחוק הזזת את הסטיק:

```text
0-5%      = deadzone
5-35%     = step 1, repeat every 120ms
35-70%    = step 3, repeat every 70ms
70-100%   = step 10, repeat every 40ms
```

## תפריט OLED

- KEY0 לחיצה קצרה: מעבר לשדה הבא
- KEY1 לחיצה קצרה: הגדלת הערך / שינוי תו
- KEY1 לחיצה ארוכה: שמירה, עדכון IP, ופתיחה מחדש של Telnet
- KEY0+KEY1 לחיצה ארוכה יחד: החזרת הגדרות ברירת מחדל

השדות שניתנים לעריכה:

```text
Pico IP octets
MA2 IP octets
Username chars 0-15
Password chars 0-15
Deadzone
Speed 1/2 boundary
Speed 2/3 boundary
```

עריכת שם משתמש/סיסמה עם שני כפתורים לא נוחה, אבל היא עובדת. ברירת המחדל אמורה להספיק ברוב המקרים אם MA2 משתמש ב-administrator בלי סיסמה.

## חשוב

זו גרסת מקור ניסיונית ל-GitHub Actions. היא מיועדת ל-Pico 2 W רגיל, לא Plus, ובגלל שהורדנו אודיו/גיימפד/מידי היא אמורה להיות הרבה יותר קלה בזיכרון מהדונגל המלא.

אם Windows לא מזהה את ההתקן כרשת, ייתכן שצריך לבחור/להתקין RNDIS driver ידנית במנהל ההתקנים.
