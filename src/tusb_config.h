#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Pico SDK passes CFG_TUSB_MCU on the compiler command line for the selected
// board. Guard it to avoid redefinition warnings on Pico SDK 2.x.
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU                 OPT_MCU_RP2350
#endif

#define CFG_TUSB_OS                  OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE        (OPT_MODE_DEVICE)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN           __attribute__ ((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE       64

#define CFG_TUD_CDC                  0
#define CFG_TUD_MSC                  0
#define CFG_TUD_HID                  0
#define CFG_TUD_MIDI                 0
#define CFG_TUD_VENDOR               0
#define CFG_TUD_AUDIO                0

// TinyUSB 0.20 renamed CFG_TUD_NET to CFG_TUD_ECM_RNDIS.
// Use the new name so the build does not emit net-class warnings.
#define CFG_TUD_ECM_RNDIS            1
#define CFG_TUD_NCM                  0

#define CFG_TUD_NET_MTU              1500

#ifdef __cplusplus
}
#endif
