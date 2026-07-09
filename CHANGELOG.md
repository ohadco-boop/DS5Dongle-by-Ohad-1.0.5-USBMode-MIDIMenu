## USB Mode / MIDI update

- Added OLED Settings USB Mode: Gamepad or MIDI. Saving the setting reboots into the selected USB descriptor.
- MIDI mode exposes USB MIDI only; no HID gamepad and no USB Audio.

## 1.0.5
- Added fast hold-to-scroll for D-Pad Up/Down in Settings and Remap.
- Settings and Remap scrolling keeps wraparound behavior at the top and bottom.
- Removed Pico LED from the Settings menu and from the built-in Help.
- Pico LED is kept off by default/fixed without changing the config layout.

## 1.0.5
- Hide AudBuf from the OLED Settings menu and from the built-in Help; the config field remains internal for compatibility.
 - Trigger Test footer fix
- On the Hebrew Trigger Test screen, moved the Triangle icon before the cycle label with a clear gap so it no longer overlaps the word.

## 1.0.5 - Fast looping Help scroll


## 1.0.5 OLED polish — status/help/i18n
- Status screen: moved Triangle/Circle/Cross/Square icons 2 px right relative to the D-pad arrows.
- Status screen: moved the PS indicator 4 px right.
- Slots screen Hebrew footer: moved the Triangle icon 2 px left near the switch label.
- BT Signal screen: localized Hebrew UI while keeping the RSSI line in English.
- Touchpad screen: localized the Fingers label in Hebrew.
- Trigger Test screen: localized mode/footer text in Hebrew and removed the Hebrew "hold" wording.
- Hebrew help: fixed mixed Hebrew/English spacing so English terms do not visually attach to the following Hebrew word.
- Holding D-Pad Up or Down on the Help screen now auto-scrolls quickly instead of moving only one row per press.
- Help scrolling now wraps: moving down past the end returns to the start, and moving up above the start jumps to the end.
- The behavior is local to Help and does not affect Settings, Slots, or Remap navigation.

## 1.0.5 - Manual-style Help and Status trigger polish

- Reworked the OLED Help screen from a short shortcut list into a long built-in manual with detailed scrollable explanations for navigation, pairing, Status, Slots, Lightbar, Triggers, Gyro, Touchpad, BT Signal, VU, Remap, and Settings options.
- Kept Help read-only; it still scrolls with D-Pad Up/Down and does not touch flash, audio, or runtime settings.
- Status L2/R2 trigger bars now remain white-on-black while pressed; only button icons use pressed inversion.

## 1.0.5 - OLED Help / Status polish v2

- Expanded the OLED Help screen into a much fuller on-device guide, with about 70 scrollable `action: button/detail` rows so the dongle is less dependent on an external manual.
- Kept bullet markers at the start of each Help row in English and Hebrew for clearer separation.
- Cleaned the Status layout: removed the Mute indicator, removed the stray mark near Cross/PS, shifted the D-pad / face / PS cluster slightly left, and changed L1/R1 to equal text labels.
- Previous polish kept: Hebrew Slots footer spacing, PlayStation-style Status symbols, compact Create / Options / PS / Touchpad indicators, and arrow symbols on Remap D-pad directions.

# CHANGELOG
- Reordered OLED screen rotation so Help appears immediately after Status and before Slots.
- Reordered the Settings menu into logical groups: language/control, power/audio, haptics, display, then maintenance actions.
- Added a new OLED Help screen with a compact on-device quick guide: Pico/OLED navigation, controller navigation, saving, power-off, AudioKeep, and pairing.
- Help scrolls with D-pad Up/Down and does not write to Flash or touch audio paths.
- Hebrew popups now call the microphone `מיק שלט` instead of exposing the Pico name.
- Removed KEY0/KEY1 navigation arrows from all OLED screens and reclaimed the left OLED margin for content.
- Localized OLED full-screen popups in Hebrew UI: power-off, headset/headphones jack status, controller mic on/off, and save/fail messages.
- Removed the OLED Diagnostics screen from the regular screen rotation to reduce UI clutter and avoid unused diagnostics overhead.

### 1.0.5 Safe Flash Layout Fix v2
- Reverted the broken FlashReserveFix approach that reduced `PICO_FLASH_SIZE_BYTES`; that could make Flash writes hang and trigger watchdog during Save.
- Kept the safer layout-only approach: Settings move to `-4`, Slots move to `-5`, Remap remains at `-3`.
- No forced Flash write during active audio.


### 1.0.5 Persistent Flash Layout Fix
- Moved OLED Settings storage out of BTstack's last flash banks. Settings now use a safe app-owned sector at `PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE`.
- Kept Remap storage at `-3` because it was already outside the BTstack banks and was not the source of the reset.
- Moved Slots storage to `-5` to avoid the old `-2` BTstack overlap.
- Added one-time migration from the old Settings/Slots sectors if valid data still exists there.
- This fixes Settings resetting after flashing the same firmware again; if a previous flash already wiped the old Settings sector, set the options once more and future updates should preserve them.

### 1.0.5 Save status patch
- Save confirmation popups (`Saved!` / `Save pending` / `Save FAIL`) now auto-clear after 2 seconds so the Settings/Remap title returns.
- No forced Flash write during active audio; deferred saves remain audio-safe.

### 1.0.5 OLED polish patch
- Hebrew dirty-state marker now shows the literal `*` instead of the word `כוכב`.
- Removed the CPU / Clock OLED screen from normal screen navigation. The remaining screens flow directly from Diagnostics to BT Signal.

### 1.0.5 PersistentSettings patch
- Fixed config migration so firmware upgrades preserve existing OLED Settings instead of falling back to defaults when the schema version changes.
- New config fields are initialized safely, while existing user values are kept.


## 1.0.5 Hebrew UI Polish - Symbol patch
- Hebrew Settings labels updated: Controller Type, Mic Gain level, and Audio Wake wording.
- Settings footer now shows only the Triangle save symbol instead of long arrow instructions.
- Remap ON/OFF is localized in Hebrew UI.
- Face buttons in Remap are drawn as button symbols where possible in both English and Hebrew UI.

## 1.0.5 - Hebrew OLED UI

- Added optional Hebrew OLED UI language.
- Added `Settings -> Language` with English/Hebrew toggle.
- Added a compact Hebrew bitmap font for the 128x64 OLED.
- Localized main OLED screen titles, Settings labels, Remap title/footer, pairing hints and save/status messages.
- Refined Hebrew pairing instructions: `Create + PS` is kept readable in English while the surrounding instructions are Hebrew.
- Fixed Hebrew Lightbar header overlap between `[BATT]` and `תאורה`, and changed the footer hint to `שינוי מצב עם R1`.
- Restored fast watchdog recovery: global timeout is now 1.2 s, while planned power-off paths still feed the watchdog explicitly.
- Shortened the watchdog reboot LED indication so accidental watchdog resets recover faster.
- Default language remains English for existing users.
- Based on 1.0.4 Stable + AudioRouteFix + PoweroffTouchpadWdtFix.


## 1.0.4 - Poweroff Touchpad Watchdog Fix

- Fixed a case where touching the DualSense touchpad during controller power-off could keep high-rate HID interrupt reports flowing and trip the Pico watchdog.
- The safe power-off guard now remains active until the real HCI DISCONNECTION_COMPLETE event, instead of ending immediately after sending the disconnect command.
- Late controller interrupt reports are dropped during intentional power-off to keep Bluetooth/USB teardown quiet.
- The original AudioRouteFix is preserved. Fix2/Fix3 and Tester TTL behavior are not included.


### 1.0.4 PoweroffWatchdogFix
- Prevented false Pico watchdog resets during intentional controller power-off after long audio/game sessions.
- Kept the watchdog enabled, but extended the timeout from 1 s to 3 s and feeds it during the safe BT teardown path.
- No changes to AudioRouteFix behavior, Tester route behavior, or USB audio descriptors.


## v1.0.4 - Audio Route Fix
- Added automatic USB Audio route recovery when a browser or tester page stops the audio stream without a headset plug/unplug event.
- Fixes cases where DualSense Tester could leave audio stuck until reconnecting headphones.
- Clears stale host audio route state after the stream stops or the USB Audio interface returns to idle.
- AudioKeep and deferred Flash save now use recent effective audio activity instead of stale USB audio state.
- Output reports are no longer treated as missing speaker audio packets after a stale browser/tester stream.

## v1.0.4 - Stable AudioFix
- Fixed the AudioKeep + save + poweroff sequence.
- The firmware now sends an audio-safe mute/off state to the controller before any forced Flash write.
- Added a short delay before saving/disconnecting to prevent broken audio on the next connection.
- Cleans cached host audio route after local disconnect so the next connection starts clean.

## v1.0.4 - Stable
- Updated the firmware version to `1.0.4` across the build system and OLED fallback text.
- GitHub Actions UF2 artifact name: `DS5Dongle-by-Ohad-1.0.4.uf2`.
- Added official Boot Screen: `DS5Dongle / by Ohad / v1.0.4`.
- Status screen displays `DS5Dongle 1.0.4`.
- AudioKeep is included as an official feature: when ON, active audio prevents Idle shutdown.
- PowerCombo and Idle shutdown use a safe poweroff path and return to the Status screen before disconnecting.
- Fixed Settings item indexing after adding AudioKeep, so Reset Defaults and Wipe Slots no longer collide with AudioKeep.
- Added new OLED asset: `assets/oled/oled_sc00_boot.jpg`.
- Updated Status OLED asset for v1.0.4: `assets/oled/oled_sc01.jpg`.
- Added user manual: `USER_MANUAL.pdf` and `docs/USER_MANUAL.md`.
- Added product specification: `PRODUCT_SPECIFICATION.pdf` and `docs/PRODUCT_SPECIFICATION.md`.

## v1.0.1
- Added AudioKeep on/off: prevents Idle shutdown while audio is active.
- Added the REMAP OLED asset under `assets/oled`.

## v1.0.0
- Initial stable DS5Dongle by Ohad baseline with OLED UI, Settings, REMAP, PowerCombo, Audio, Diagnostics, and pairing slots.
