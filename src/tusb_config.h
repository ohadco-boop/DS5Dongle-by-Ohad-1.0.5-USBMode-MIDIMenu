#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ENABLE_SERIAL
#define ENABLE_SERIAL 0
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT            0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED         OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------+
// Common Configuration
//--------------------------------------------------------------------+
// Pico SDK normally passes CFG_TUSB_MCU on the compiler command line.
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU                OPT_MCU_RP2350
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG              0
#endif

// Enable Device stack explicitly. Without this some TinyUSB builds do not
// expose the network device class declarations.
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           BOARD_TUD_MAX_SPEED
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE      64

//--------------------------------------------------------------------+
// Device classes: this firmware is USB network only.
//--------------------------------------------------------------------+
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0

// TinyUSB 0.20 name. Do not also define deprecated CFG_TUD_NET.
#define CFG_TUD_ECM_RNDIS           1
#define CFG_TUD_NCM                 0
#define CFG_TUD_NET_MTU             1500

// Do not define CFG_TUD_NET_ENDPOINT_SIZE here. TinyUSB net_device.h defines it
// internally; defining it in user config creates a redefinition warning/failure
// with Pico SDK 2.2.0 + TinyUSB 0.20.0.

#ifdef __cplusplus
}
#endif
