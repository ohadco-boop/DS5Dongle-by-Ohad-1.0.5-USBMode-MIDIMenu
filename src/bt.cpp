//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstring>
#include "bt.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include "btstack_event.h"
#include "gap.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "state_mgr.h"
#include "pico/util/queue.h"
#include "slots.h"
#include "oled.h"
#include "audio.h"
#include "hardware/watchdog.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

// Connection-attempt watchdog: if a connection commits to a device (inquiry
// found one / incoming request accepted) but doesn't reach USB-enumeration
// within this window, tear down and retry. Catches the silent stalls caused by
// USB 3.0 2.4 GHz RF interference on the CYW43 BT radio (DualSense stuck on the
// amber init lightbar, never enumerates) — see README troubleshooting. A
// healthy or slow re-pair finishes well under 6 s, so 10 s never trips a real
// connection but heals before the user reaches to replug.
#define CONNECT_WATCHDOG_TIMEOUT_US (10 * 1000 * 1000)

using std::unordered_map;
using std::vector;
using std::queue;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;
static bool device_found = false;
static bool new_pair = false; // 只有新匹配的设备才用创建channel，自动重连走的是service
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = nullptr;
static bool check_dse = false;
static int8_t bt_rssi = 0;
unordered_map<uint8_t, vector<uint8_t> > feature_data;
queue_t send_fifo;

// fixed65: short output guard. Used only around reconnect / first host OUT burst.
// It intentionally DROPS output packets instead of queueing them so stale
// rumble/adaptive-trigger/lightbar init packets from a game do not hit the
// controller while the BT link is still settling. Feature reports/control path
// are left untouched.
static absolute_time_t g_output_guard_until = 0;

struct send_element {
    uint8_t data[512];
    size_t len;
};

absolute_time_t inactive_time = 0; // 手柄长时间静默

// fixed65t: auto-disconnect idle detection must be based only on real
// physical controller input. Some games/testers keep the DualSense audio/mic
// or output path active and the controller may send non-input interrupt frames
// (for example mic/audio-tagged 0x31 frames). Those frames must not refresh the
// idle timer, otherwise the controller never powers off while a game is open.
static bool bt_packet_is_standard_ds5_input(const uint8_t *packet, uint16_t size) {
    if (size < 66) return false;              // A1 31 flags + 63-byte input report
    if (packet[0] != 0xA1 || packet[1] != 0x31) return false;
    if (((packet[2] >> 1) & 1) && size >= 75) return false; // mic/audio Opus frame
    return true;
}

static bool bt_packet_physical_input_is_idle(const uint8_t *packet, uint16_t size) {
    if (!bt_packet_is_standard_ds5_input(packet, size)) return false;

    const uint8_t *r = packet + 3; // same 63-byte report copied to interrupt_in_data

    const bool sticks_idle =
        r[0] >= 120 && r[0] <= 140 &&
        r[1] >= 120 && r[1] <= 140 &&
        r[2] >= 120 && r[2] <= 140 &&
        r[3] >= 120 && r[3] <= 140;
    const bool triggers_idle = (r[4] == 0 && r[5] == 0);
    const bool dpad_idle = ((r[7] & 0x0F) == 0x08);
    const bool face_idle = ((r[7] & 0xF0) == 0x00);
    const bool shoulder_options_idle = (r[8] == 0x00);
    const bool ps_touch_mic_idle = (r[9] == 0x00);

    return sticks_idle && triggers_idle && dpad_idle && face_idle &&
           shoulder_options_idle && ps_touch_mic_idle;
}

// Connection-attempt watchdog timestamp. 0 == not armed; armed == a connection
// attempt is in flight (committed to a device, not yet USB-enumerating). Set
// when an attempt begins, cleared the instant the controller type is identified
// (USB connects) and on every teardown. Checked by bt_connection_watchdog_tick().
static absolute_time_t connect_attempt_started = 0;

// Multi-slot pairing state. Modeled on zurce/DS5Dongle-OLED.
static int g_current_slot = 0;

bool bt_disconnect();  // fwd decl — defined further down
// fixed65v: route idle auto-poweroff through the same safe save/OLED path as PS+Options.
extern void controller_poweroff_request();
extern bool controller_poweroff_is_pending();
extern void controller_poweroff_note_bt_disconnected();

// Keep the dongle discoverable while at least one slot is empty (covers
// initial setup + partial-wipe states). Once all 4 slots are full, go
// non-discoverable so a stray phone can't try to pair.
static void update_discoverable() {
    if (slots_any_empty()) {
        gap_discoverable_control(1);
    } else {
        gap_discoverable_control(0);
    }
}

void bt_register_data_callback(bt_data_callback_t callback) {
    bt_data_callback = callback;
}

void bt_flush_send_fifo() {
    send_element dropped{};
    while (queue_try_remove(&send_fifo, &dropped)) {
        // Drain stale output.
    }
}

void bt_output_guard_start_ms(uint32_t ms) {
    // fixed65g: ms==0 means disabled/off for this event. Do not clear an
    // already-active guard, because the BT-connect and OUT-burst guards share
    // the same drop window and a disabled OUT guard must not cancel BT guard.
    if (ms == 0) return;

    // Non-zero values are clamped to the OLED menu range: 100..4000 ms.
    if (ms < 100) ms = 100;
    if (ms > 4000) ms = 4000;

    bt_flush_send_fifo();

    const absolute_time_t now = get_absolute_time();
    const absolute_time_t new_until = make_timeout_time_ms(ms);

    // If another guard is already active, never shorten it. This keeps a
    // 2000 ms BT-connect guard from being overwritten by a 500 ms OUT guard.
    if (g_output_guard_until != 0 && absolute_time_diff_us(now, g_output_guard_until) > 0) {
        if (absolute_time_diff_us(g_output_guard_until, new_until) > 0) {
            g_output_guard_until = new_until;
        }
    } else {
        g_output_guard_until = new_until;
    }
}

bool bt_output_guard_active() {
    if (g_output_guard_until == 0) return false;
    if (absolute_time_diff_us(get_absolute_time(), g_output_guard_until) > 0) {
        return true;
    }
    g_output_guard_until = 0;
    return false;
}

// ---- OLED add-on + multi-slot accessors --------------------------------

bool bt_is_connected() { return hid_interrupt_cid != 0; }

void bt_get_addr(uint8_t out[6]) { memcpy(out, current_device_addr, 6); }

uint32_t bt_hci_err_count() { return 0; }  // stub for OLED Diagnostics

int bt_get_slot() { return g_current_slot; }

void bt_set_slot(int slot) {
    if (slot < 0 || slot >= kNumSlots) return;
    if (slot == g_current_slot) return;
    g_current_slot = slot;

    Config_body cfg = get_config();
    cfg.current_slot = (uint8_t)slot;
    set_config(cfg);
    config_save();

    if (bt_is_connected()) {
        // DISCONNECTION_COMPLETE will restart inquiry under the new filter.
        bt_disconnect();
    } else {
        gap_inquiry_stop();
        gap_inquiry_start(30);
    }
    update_discoverable();
}

bool bt_slot_occupied(int slot) { return slot_occupied(slot); }
void bt_slot_get_addr(int slot, uint8_t out[6]) { slot_get_addr(slot, out); }

void bt_forget_slot(int slot) {
    if (slot < 0 || slot >= kNumSlots) return;
    if (slot_occupied(slot)) {
        uint8_t addr[6];
        slot_get_addr(slot, addr);
        gap_drop_link_key_for_bd_addr(addr);
    }
    slot_forget(slot);
    update_discoverable();
    if (slot == g_current_slot && bt_is_connected()) {
        bt_disconnect();
    }
}

void bt_wipe_all_slots() {
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t snapshot[16];
        int n = 0;
        bd_addr_t addr;
        link_key_t key;
        link_key_type_t type;
        while (n < 16 && gap_link_key_iterator_get_next(&it, addr, key, &type)) {
            bd_addr_copy(snapshot[n++], addr);
        }
        gap_link_key_iterator_done(&it);
        for (int i = 0; i < n; i++) {
            gap_drop_link_key_for_bd_addr(snapshot[i]);
        }
    }
    slots_wipe_all();
    update_discoverable();
    if (bt_is_connected()) {
        bt_disconnect();
    }
}

void bt_send_packet(uint8_t *data, uint16_t len) {
    if (hid_interrupt_cid != 0) {
        l2cap_send(hid_interrupt_cid, data, len);
    }
}

void bt_send_control(uint8_t *data, uint16_t len) {
    if (hid_control_cid != 0) {
        l2cap_send(hid_control_cid, data, len);
    }
}

bool bt_disconnect() {
#if !ENABLE_SERIAL
    watchdog_update();
#endif
    // fixed65: a manual/controller disconnect makes every queued output stale.
    bt_flush_send_fifo();
    g_output_guard_until = 0;
    if (acl_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }

    // 0x13 = remote user terminated connection
    hci_send_cmd(&hci_disconnect, acl_handle, 0x13);
#if !ENABLE_SERIAL
    watchdog_update();
#endif
    return true;
}

// Called every main-loop iteration. If a connection attempt has stalled past
// the timeout, tear it down so the state machine retries instead of hanging
// (e.g. on the amber lightbar under USB 3.0 RF interference). Inert unless a
// connection attempt is in flight, so it never touches a healthy session.
void bt_connection_watchdog_tick() {
    if (connect_attempt_started == 0) return; // not armed
    if (absolute_time_diff_us(connect_attempt_started, get_absolute_time())
            < CONNECT_WATCHDOG_TIMEOUT_US) {
        return;
    }
    printf("[BT] Connection watchdog: attempt stalled, recovering\n");
    connect_attempt_started = 0; // disarm; the next attempt re-arms

    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        // ACL is up but setup stalled (auth/encryption/L2CAP/feature-wait).
        // Route through the proven HCI_EVENT_DISCONNECTION_COMPLETE teardown.
        bt_disconnect();
    } else {
        // No ACL yet (stalled before/at create-connection) — reset by hand
        // and kick a fresh inquiry.
        device_found = false;
        new_pair = false;
        gap_inquiry_stop();
        gap_inquiry_start(30);
        gap_connectable_control(1);
        update_discoverable();
    }
}

void bt_get_signal_strength(int8_t *rssi) {
    // gap_read_rssi() completes asynchronously, so this function can only
    // return the last cached RSSI value. Trigger a refresh afterwards so a
    // subsequent call can observe the updated value once the RSSI event arrives.
    if (rssi != nullptr) {
        *rssi = bt_rssi;
    }
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(acl_handle);
    }
}

void bt_l2cap_init() {
    l2cap_event_callback_registration.callback = &l2cap_packet_handler;
    l2cap_add_event_handler(&l2cap_event_callback_registration);
    // 修复重连后自动断开的关键点
    sdp_init();
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL, MTU_CONTROL, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT, MTU_INTERRUPT, LEVEL_2);

    l2cap_init();
}

int bt_init() {
    queue_init(&send_fifo, sizeof(send_element), 10);

    // Load persistent slot table BEFORE HCI comes up so the inquiry filter
    // and discoverable-gating see the right state on the first event.
    slots_load();
    g_current_slot = get_config().current_slot;
    if (g_current_slot < 0 || g_current_slot >= kNumSlots) g_current_slot = 0;
    printf("[BT] Boot slot = %d\n", g_current_slot);

    bt_l2cap_init();

    // SSP (Secure Simple Pairing)
    gap_ssp_set_enable(true);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);

    gap_connectable_control(1);
    update_discoverable();

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

/*int main() {
    stdio_init_all();

    /*while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    printf("USB Serial connected!\n");#1#

    bt_init();

    while (1) {
        sleep_ms(10);
    }
}*/

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
                printf("[BT] Stack ready, start inquiry\n");
                gap_inquiry_start(30);
            }
            break;
        }
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE: {
            bd_addr_t addr;
            uint32_t cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                cod = hci_event_inquiry_result_get_class_of_device(packet);
                hci_event_inquiry_result_get_bd_addr(packet, addr);
            } else if (event_type == HCI_EVENT_INQUIRY_RESULT_WITH_RSSI) {
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
            } else {
                cod = hci_event_extended_inquiry_response_get_class_of_device(packet);
                hci_event_extended_inquiry_response_get_bd_addr(packet, addr);
            }

            // CoD 0x002508 = Gamepad (Major: Peripheral, Minor: Gamepad)
            if ((cod & 0x000F00) == 0x000500) {
                // Slot-ownership filter: skip devices owned by a different slot;
                // if our slot is occupied, only accept its exact bd_addr.
                // Unowned devices pair into the current slot if it's empty.
                const int owner = slot_owner_of(addr);
                if (owner >= 0 && owner != g_current_slot) {
                    printf("[HCI] Gamepad %s belongs to slot %d, skip (cur=%d)\n",
                           bd_addr_to_str(addr), owner, g_current_slot);
                    break;
                }
                if (slot_occupied(g_current_slot)) {
                    uint8_t want[6];
                    slot_get_addr(g_current_slot, want);
                    if (memcmp(want, addr, 6) != 0) {
                        printf("[HCI] Slot %d wants different addr, skip %s\n",
                               g_current_slot, bd_addr_to_str(addr));
                        break;
                    }
                }
                printf("[HCI] Gamepad found: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int) cod);
                bd_addr_copy(current_device_addr, addr);
                device_found = true;
                gap_inquiry_stop();
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete.\n");
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                new_pair = true;
                connect_attempt_started = get_absolute_time(); // arm connection watchdog
                hci_send_cmd(&hci_create_connection, current_device_addr,
                             hci_usable_acl_packet_types(), 0, 0, 0, 1);
                break;
            }
            if (event_type == HCI_EVENT_INQUIRY_COMPLETE) {
                printf("[HCI] Restart inquiry\n");
                gap_inquiry_start(30);
                gap_connectable_control(1);
                update_discoverable();
            }
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[HCI] CmdStatus %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION && status != ERROR_CODE_SUCCESS) {
                device_found = false;
                new_pair = false;
                connect_attempt_started = 0; // disarm; failed before an ACL existed
                printf("[HCI] Create connection rejected, restart inquiry\n");
                gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (opcode != HCI_OPCODE_HCI_READ_RSSI) {
                printf("[HCI] CmdComplete %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            }
            if (opcode == HCI_OPCODE_HCI_READ_RSSI) {
                if (status != ERROR_CODE_SUCCESS || packet[1] < 7) {
                    printf("[HCI] RSSI complete failed status=0x%02X param_len=%u\n", status, packet[1]);
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                acl_handle = handle;
                bt_rssi = 0;
                hci_event_connection_complete_get_bd_addr(packet, current_device_addr);
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
                printf("[HCI] Request authentication on handle=0x%04X\n", handle);
                hci_send_cmd(&hci_authentication_requested, handle);
            } else {
                device_found = false;
                new_pair = false;
                connect_attempt_started = 0; // disarm; no ACL was established
                printf("[HCI] ACL connect failed status=0x%02X, restart inquiry\n", status);
                gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_t link_key;
            link_key_type_t link_key_type;
            bool link = gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type);
            printf("[HCI] Link key: ");
            for (int i = 0; i < sizeof(link_key_t); i++) {
                printf("%02X", link_key[i]);
            }
            printf("\n");
            if (link) {
                printf("[HCI] Link key request from %s, reply stored key type=%u\n", bd_addr_to_str(addr),
                       (unsigned int) link_key_type);
                hci_send_cmd(&hci_link_key_request_reply, addr, link_key);
            } else {
                printf("[HCI] Link key request from %s, no key, force re-pair\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_link_key_request_negative_reply, addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("[HCI] User confirmation request from %s, accept\n", bd_addr_to_str(addr));
            hci_send_cmd(&hci_user_confirmation_request_reply, addr);
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("[HCI] Legacy pin request from %s, reply 0000\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            const uint8_t status = hci_event_authentication_complete_get_status(packet);
            const hci_con_handle_t handle = hci_event_authentication_complete_get_connection_handle(packet);
            printf("[HCI] Authentication complete handle=0x%04X status=0x%02X\n", handle, status);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[HCI] Authentication failed, drop stored key for %s\n", bd_addr_to_str(current_device_addr));
                gap_drop_link_key_for_bd_addr(current_device_addr);
                connect_attempt_started = 0; // disarm; teardown below re-inquires
                // ACL is still up — route through the clean disconnect path
                // (HCI_EVENT_DISCONNECTION_COMPLETE restarts inquiry) rather
                // than leaving a half-open ACL.
                bt_disconnect();
            } else {
                hci_send_cmd(&hci_set_connection_encryption, handle, 1);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            const uint8_t status = hci_event_encryption_change_get_status(packet);
            const hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            const uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            printf("[HCI] Encryption change handle=0x%04X status=0x%02X enabled=%u\n", handle, status, enabled);
            if (status == ERROR_CODE_SUCCESS && enabled) {
                printf("[L2CAP] Open HID channels\n");
                if (new_pair) {
                    if (hid_control_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_CONTROL, MTU_CONTROL,
                                             &hid_control_cid);
                    } else if (hid_interrupt_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_INTERRUPT,
                                             MTU_INTERRUPT,
                                             &hid_interrupt_cid);
                    }
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            if ((cod & 0x000F00) == 0x000500) {
                bd_addr_copy(current_device_addr, addr);
                gap_inquiry_stop();
                hci_send_cmd(&hci_accept_connection_request, addr, 0x01);
                connect_attempt_started = get_absolute_time(); // arm watchdog (incoming path)
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
#if !ENABLE_SERIAL
            watchdog_update();
            tud_disconnect();
            watchdog_update();
#endif
            gap_connectable_control(1);
            update_discoverable();
            const uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            device_found = false;
            new_pair = false;
            connect_attempt_started = 0; // disarm — every teardown clears here
            controller_poweroff_note_bt_disconnected();
            acl_handle = HCI_CON_HANDLE_INVALID;
            bt_rssi = 0;
            hid_control_cid = 0;
            hid_interrupt_cid = 0;
            bt_flush_send_fifo();
#if !ENABLE_SERIAL
            watchdog_update();
#endif
            g_output_guard_until = 0;
            feature_data.clear();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
#if ENABLE_BATT_LED
            battery_led_on_disconnect();
#endif
            printf("[HCI] Disconnected reason=0x%02X, start inquiry\n", reason);
#if !ENABLE_SERIAL
            watchdog_update();
#endif
            gap_inquiry_start(30);
#if !ENABLE_SERIAL
            watchdog_update();
#endif
            break;
        }

        case GAP_EVENT_RSSI_MEASUREMENT: {
            const hci_con_handle_t handle = gap_event_rssi_measurement_get_con_handle(packet);
            if (handle == acl_handle) {
                bt_rssi = static_cast<int8_t>(gap_event_rssi_measurement_get_rssi(packet));
            }
            break;
        }
    }
}

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel == hid_interrupt_cid) {
            // During local safe power-off, drop late controller interrupt input
            // immediately. A finger on the touchpad can generate a high-rate
            // stream while hci_disconnect() is pending; processing those reports
            // during teardown is unnecessary and was observed to trip the Pico
            // watchdog. HCI/L2CAP teardown events are still processed normally.
            if (controller_poweroff_is_pending()) {
                return;
            }
            // printf("[L2CAP] HID Interrupt data len=%u\n", size);
            // printf_hexdump(packet, size);
            bt_data_callback(INTERRUPT, packet, size);

            // fixed65t idle auto-disconnect:
            // Count only physical button/stick/trigger activity from standard
            // 0x31 input reports. Ignore host/game-driven interrupt traffic
            // such as mic/audio frames so an open game cannot keep resetting
            // the idle timer forever.
            if (get_config().disable_inactive_disconnect) {
                return;
            }
            if (bt_packet_is_standard_ds5_input(packet, size)) {
                if (!bt_packet_physical_input_is_idle(packet, size)) {
                    inactive_time = get_absolute_time();
                } else if (absolute_time_diff_us(inactive_time, get_absolute_time()) >
                           static_cast<int64_t>(get_config().inactive_time) * 60 * 1000 * 1000) {
                    // DS5Dongle by Ohad 1.0.4: optional AudioKeep. If the host is
                    // actively streaming USB audio, keep the controller connected
                    // so it can be used as a wireless music/headset endpoint. The
                    // normal idle poweroff resumes once audio is no longer active.
                    if (get_config().keep_awake_on_audio && audio_usb_active()) {
                        inactive_time = get_absolute_time();
                        printf("idle timeout skipped: USB audio active\n");
                    } else {
                        // fixed65v: idle timeout should behave like the manual
                        // PowerCombo shutdown: show Powering Off..., commit pending
                        // OLED/settings/remap/lightbar changes, wait briefly, then
                        // disconnect. Do not call bt_disconnect() directly here.
                        printf("idle timeout: safe controller poweroff\n");
                        inactive_time = get_absolute_time();
                        controller_poweroff_request();
                    }
                }
            }
        } else if (channel == hid_control_cid) {
            if (check_dse) {
                if (packet[0] == 0xA3 && packet[1] == 0x70) {
                    printf("Connected DSE Controller\n");
                    check_dse = false;
                    is_dse = true;
                    connect_attempt_started = 0; // fully up — disarm watchdog
#if !ENABLE_SERIAL
                    tud_connect();
#endif
                } else if (packet[0] == 0x02) {
                    printf("Connected DS5 Controller\n");
                    check_dse = false;
                    is_dse = false;
                    connect_attempt_started = 0; // fully up — disarm watchdog
#if !ENABLE_SERIAL
                    tud_connect();
#endif
                }
            }
            if (packet[0] == 0xA3) {
                uint8_t report_id = packet[1];
                feature_data[report_id].assign(packet + 1, packet + size);
#if ENABLE_VERBOSE
                printf("[L2CAP] Stored Feature Report 0x%02X, len=%u\n", report_id, size - 1);
#endif
            }
#if ENABLE_VERBOSE
            printf("[L2CAP] HID Control data len=%u\n", size);
            printf_hexdump(packet, size);
#endif
            bt_data_callback(CONTROL, packet, size);
        } else {
            printf("[L2CAP] Data on unknown channel 0x%04X (Interrupt: 0x%04X, Control: 0x%04X)\n",
                   channel, hid_interrupt_cid, hid_control_cid);
        }
        return;
    }

    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            const uint8_t status = l2cap_event_channel_opened_get_status(packet);
            const uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
            if (status == 0) {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                if (psm == PSM_HID_CONTROL) {
                    printf("[L2CAP] HID Control opened cid=0x%04X\n", local_cid);
                    hid_control_cid = local_cid;

                    // First-time pairing: assign this bd_addr to the current slot.
                    if (!slot_occupied(g_current_slot)) {
                        slot_assign(g_current_slot, current_device_addr);
                        printf("[Slots] Assigned %s to slot %d\n",
                               bd_addr_to_str(current_device_addr), g_current_slot);
                        update_discoverable();
                    }

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_control_cid);
                    printf("[L2CAP] Remote Control MTU: %d\n",mtu);
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X\n", local_cid);
                    hid_interrupt_cid = local_cid;

                    if (!get_config().disable_pico_led) {
                        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
                    }
                    inactive_time = get_absolute_time();

                    printf("Init DualSense\n");

                    init_feature();
                    // 初始化手柄状态
                    uint8_t report32[142]{};
                    report32[0] = 0x32;
                    report32[1] = 0x10; // reportSeqCounter
                    report32[2] = 0x10 | 0 << 6 | 1 << 7;
                    report32[3] = 0x3f; // 63 bytes
                    state_set(report32 + 4,sizeof(SetStateData));
                    bt_write(report32, sizeof(report32));

                    // fixed65ak: BT connect guard removed. Keep the controller
                    // init packet above, then forward host output normally.

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_interrupt_cid);
                    printf("[L2CAP] Remote Interrupt MTU: %d\n",mtu);

                    // OLED Edition: keep discoverable rule centralized — discoverable
                    // when any slot is empty, dark otherwise.
                    update_discoverable();
                    // tud_connect();
                } else {
                    printf("[L2CAP] Unknown Channel psm: 0x%02X", psm);
                }

                /*if (hid_control_cid != 0 && hid_interrupt_cid != 0) {
                    printf("[L2CAP] HID channels ready, request CAN_SEND_NOW for SET_PROTOCOL\n");
                    l2cap_request_can_send_now_event(hid_control_cid);
                }*/
            } else {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                hid_control_cid = 0;
                hid_interrupt_cid = 0;
                device_found = false;
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X\n", psm, status);
                bt_disconnect();
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            const uint16_t local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
            const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X\n", psm, local_cid);
            l2cap_accept_connection(local_cid);
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            const uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
            if (local_cid == hid_control_cid) {
                hid_control_cid = 0;
                printf("[L2CAP] HID Control closed cid=0x%04X\n", local_cid);
            } else if (local_cid == hid_interrupt_cid) {
                hid_interrupt_cid = 0;
                printf("[L2CAP] HID Interrupt closed cid=0x%04X\n", local_cid);
            } else {
                printf("[L2CAP] Channel closed cid=0x%04X\n", local_cid);
            }
            if (hid_control_cid == 0 && hid_interrupt_cid == 0) {
                bt_flush_send_fifo();
                g_output_guard_until = 0;
                bt_disconnect();
            }
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            // printf("[L2CAP] L2CAP_EVENT_CAN_SEND_NOW\n");

            send_element send_packet{};
            if (queue_try_remove(&send_fifo, &send_packet)) {
                const uint8_t status = l2cap_send(hid_interrupt_cid, send_packet.data, send_packet.len);
                if (status != 0) {
                    printf("[L2CAP] L2CAP Send Error, Status: 0x%02X\n", status);
                }
            }
            if (!queue_is_empty(&send_fifo)) {
                l2cap_request_can_send_now_event(hid_interrupt_cid);
            }
            break;
        }
    }
}

void bt_write(const uint8_t *data, const uint16_t len) {
    if (hid_interrupt_cid == 0) return;
    if (bt_output_guard_active()) {
        // Drop during short guard; do not queue stale native-haptics/LED init.
        return;
    }
    static send_element packet{};
    memset(packet.data, 0, 512);
    packet.len = len + 1;
    packet.data[0] = 0xA2;
    memcpy(packet.data + 1, data, len);
    fill_output_report_checksum(packet.data + 1, len);

    if (!queue_try_add(&send_fifo, &packet)) {
        printf("[L2CAP bt_write] Error: Failed to add packet to send FIFO\n");
        return;
    }
    if (queue_get_level(&send_fifo) == 1) {
        l2cap_request_can_send_now_event(hid_interrupt_cid);
    }
}

vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len) {
    // 若为0x81则会请求新内容，其他若有旧数据则不进行请求
    auto ret = vector<uint8_t>{};
    if (feature_data.contains(reportId)) {
        ret = feature_data[reportId];
    }
    if (!feature_data.contains(reportId) ||
        // Get Test Command Result
        reportId == 0x81 ||
        // DSE: Set Profile Save?
        reportId == 0x63 ||
        reportId == 0x65 ||
        reportId == 0x64
    ) {
        if (hid_control_cid != 0) {
            uint8_t get_feature[] = {0x43, reportId};
            l2cap_send(hid_control_cid, get_feature, sizeof(get_feature));
#if ENABLE_VERBOSE
            printf("[L2CAP] Requesting Get Feature Report 0x%02X\n", reportId);
#endif
        }
    }
    return ret;
}

std::vector<uint8_t> bt_peek_feature(uint8_t reportId) {
    auto it = feature_data.find(reportId);
    return (it != feature_data.end()) ? it->second : std::vector<uint8_t>{};
}

void set_feature_data(uint8_t reportId, uint8_t *data, uint16_t len) {
    if (hid_control_cid != 0) {
        uint8_t get_feature[len + 2];
        get_feature[0] = 0x53;
        get_feature[1] = reportId;
        memcpy(get_feature + 2, data, len);
        fill_feature_report_checksum(get_feature + 1, len + 1);
        l2cap_send(hid_control_cid, get_feature, len + 2);
#if ENABLE_VERBOSE
        printf("[L2CAP] Requesting Set Feature Report 0x%02X\n", reportId);
        printf_hexdump(get_feature, len + 2);
#endif
    }
}

void init_feature() {
    get_feature_data(0x09, 20);
    get_feature_data(0x20, 64);
    get_feature_data(0x22, 64);
    get_feature_data(0x05, 41);
    // DSE
    // check DSE by request 0x70 feature report. DSE return DEFAULT
    // If len == 1, it's DS5
    check_dse = true;
    get_feature_data(0x70, 64);
}
