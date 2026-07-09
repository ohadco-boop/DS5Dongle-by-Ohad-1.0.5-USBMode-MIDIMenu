#include "tusb.h"
#include <cstring>

#define USB_VID 0xCafe
#define USB_PID 0x4020
#define USB_BCD 0x0100

enum {
    ITF_NUM_NET_CTRL = 0,
    ITF_NUM_NET_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
};

#define EPNUM_NET_NOTIF  0x81
#define EPNUM_NET_OUT    0x02
#define EPNUM_NET_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN)

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1
};

extern "C" uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&desc_device; }

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_RNDIS_DESCRIPTOR(ITF_NUM_NET_CTRL, STRID_INTERFACE, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE),
};

extern "C" uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

static const char* string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Ohad DS5",
    "DS5 MA2 Telnet USBNet",
    "MA2TELNET01",
    "RNDIS USB Network",
};

static uint16_t _desc_str[64];

extern "C" uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    uint8_t chr_count;

    if (index == 0) {
        std::memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return nullptr;
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t)std::strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
