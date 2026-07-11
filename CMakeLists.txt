cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32)
  set(USERHOME $ENV{USERPROFILE})
else()
  set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
  include(${picoVscode})
endif()

# This firmware targets the regular Raspberry Pi Pico 2 W.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)
project(ds5-ma2-telnet-usbnet C CXX ASM)
pico_sdk_init()

add_executable(ds5-ma2-telnet-usbnet
  src/main.cpp
  src/bt.cpp
  src/config.cpp
  src/slots.cpp
  src/state_mgr.cpp
  src/telnet_settings.cpp
  src/ma2_telnet.cpp
  src/ma2_oled.cpp
  src/usb_net_lwip.cpp
  src/usb_descriptors.cpp
  src/app_stubs.cpp
  src/ram_mem.c
)

set(VERSION "0.1.6" CACHE STRING "Program version string")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" VERSION_SHORT "${VERSION}")
if(NOT VERSION_SHORT)
  set(VERSION_SHORT "${VERSION}")
endif()

target_include_directories(ds5-ma2-telnet-usbnet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# TinyUSB RNDIS/ECM network class needs the helper files from lib/networking.
# Pico SDK 2.2.0 + TinyUSB 0.20.0 compiles ecm_rndis_device.c, which includes
# rndis_protocol.h from this folder and expects rndis_reports.c to provide
# rndis_class_set_handler(). Without this, GitHub Actions fails with:
#   fatal error: rndis_protocol.h: No such file or directory
set(TINYUSB_NETWORKING_DIR "${PICO_SDK_PATH}/lib/tinyusb/lib/networking")
if(EXISTS "${TINYUSB_NETWORKING_DIR}/rndis_reports.c")
  target_sources(ds5-ma2-telnet-usbnet PRIVATE
    "${TINYUSB_NETWORKING_DIR}/rndis_reports.c"
  )
  target_include_directories(ds5-ma2-telnet-usbnet PRIVATE
    "${TINYUSB_NETWORKING_DIR}"
  )
else()
  message(FATAL_ERROR "TinyUSB networking helpers not found: ${TINYUSB_NETWORKING_DIR}")
endif()

target_compile_definitions(ds5-ma2-telnet-usbnet PRIVATE
  FIRMWARE_VERSION="${VERSION}"
  FIRMWARE_VERSION_SHORT="${VERSION_SHORT}"
  ENABLE_SERIAL=1
  ENABLE_VERBOSE=0
  ENABLE_BATT_LED=0
  DISABLE_SPEAKER_PROC=1
  SYS_CLOCK_KHZ=150000
  CYW43_PIO_CLOCK_DIV_INT=2
  PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
  PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
  PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
)

target_compile_options(ds5-ma2-telnet-usbnet PRIVATE -fsigned-char)
set_source_files_properties(src/ram_mem.c PROPERTIES COMPILE_OPTIONS
    "-fno-builtin;-fno-tree-loop-distribute-patterns")

set_target_properties(ds5-ma2-telnet-usbnet PROPERTIES OUTPUT_NAME "ds5-ma2-telnet-usbnet")
pico_set_program_name(ds5-ma2-telnet-usbnet "DS5 MA2 Telnet USBNet")
pico_set_program_version(ds5-ma2-telnet-usbnet "${VERSION}")

pico_enable_stdio_uart(ds5-ma2-telnet-usbnet 1)
pico_enable_stdio_usb(ds5-ma2-telnet-usbnet 0)

target_link_libraries(ds5-ma2-telnet-usbnet
  pico_stdlib
  hardware_watchdog
  hardware_timer
  hardware_flash
  hardware_spi
  pico_btstack_classic
  pico_btstack_cyw43
  pico_cyw43_arch_lwip_poll
  pico_lwip_nosys
  tinyusb_device
  tinyusb_board
)

pico_add_extra_outputs(ds5-ma2-telnet-usbnet)
