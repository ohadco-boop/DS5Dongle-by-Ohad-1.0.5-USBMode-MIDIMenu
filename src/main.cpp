#include <cstdio>
#include <cstring>
#include "bsp/board_api.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "bt.h"
#include "config.h"
#include "slots.h"
#include "state_mgr.h"
#include "oled.h"
#include "telnet_settings.h"
#include "usb_net_lwip.h"
#include "ma2_telnet.h"

#ifndef SYS_CLOCK_KHZ
#define SYS_CLOCK_KHZ 150000
#endif

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

volatile uint32_t g_ds_reports = 0;
volatile uint32_t g_ds_mic_frames_dropped = 0;

static void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    if (channel != INTERRUPT || len < 4) return;

    // DualSense BT input report 0x31: data+3 contains the 63-byte USB-style input report.
    // Mic-tagged frames contain Opus audio after the header, so ignore their payload here.
    if (data[1] == 0x31 && len >= 66) {
        if (((data[2] >> 1) & 1) && len >= 75) {
            g_ds_mic_frames_dropped++;
            return;
        }
        std::memcpy(interrupt_in_data, data + 3, 63);
        g_ds_reports++;
        ma2_remote_process_report(interrupt_in_data);
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    board_init();
    // Use the legacy no-argument TinyUSB init for Pico SDK compatibility.
    // The rhport/speed version depends on TinyUSB API details and broke builds
    // on the GitHub Actions Pico SDK/TinyUSB combination.
    tusb_init();
    board_init_after_tusb();

    stdio_uart_init_full(uart0, 115200, 0, 1);
    printf("DS5 MA2 Telnet USBNet boot\n");

    if (cyw43_arch_init()) {
        printf("CYW43 init failed\n");
        return 1;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    config_load();      // Needed by the reused BT pairing/slot layer.
    telnet_settings_load();
    state_init();
    usb_net_lwip_init();
    ma2_telnet_init();
    oled_init();

    bt_init();
    bt_register_data_callback(on_bt_data);

    watchdog_enable(2500, true);

    while (true) {
        watchdog_update();
        cyw43_arch_poll();
        bt_connection_watchdog_tick();
        tud_task();
        usb_net_lwip_tick();
        ma2_telnet_tick();
        ma2_remote_tick();
        oled_loop();
        config_service_deferred_save();
    }
}
