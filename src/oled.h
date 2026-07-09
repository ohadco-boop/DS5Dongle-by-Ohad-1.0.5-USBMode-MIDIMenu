#ifndef DS5_BRIDGE_OLED_H
#define DS5_BRIDGE_OLED_H

#include <cstdint>

void oled_init();
void oled_loop();
void oled_show_message(const char *msg, uint32_t duration_ms);

// Return the UI to the main Status screen. Used after local controller
// power-off/idle disconnect so the dongle is ready on Status for reconnect.
void oled_return_to_status_screen();
// Commit any unsaved OLED-side settings/remap/lightbar edits before a local
// controller power-off. Keeps PS+Options from disconnecting the BT controller
// before pending UI changes reach flash.
bool oled_save_pending_changes_for_poweroff();
// Global physical-controller shortcut: hold Options + D-pad Left/Right to
// page OLED screens. Uses raw incoming DS5 input before remap so it still
// works when those controls are remapped for the host. Returns true while
// the shortcut frame should be swallowed from USB HID forwarding.
bool oled_handle_controller_screen_nav_shortcut();
// true only when host/game is allowed to own the DualSense lightbar.
bool oled_lightbar_host_mode();

#endif // DS5_BRIDGE_OLED_H
