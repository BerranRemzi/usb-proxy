# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
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
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# ─── Pico SDK ────────────────────────────────────────────────────────────────
# Set PICO_SDK_PATH before configuring:
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -B build ..
if(DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Target board: "pico" for Pico 1, "pico_w" for Pico W
set(PICO_BOARD pico CACHE STRING "Pico board type")

project(disney_infinity_proxy C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

option(PROXY_ENABLE_DIAGNOSTICS "Enable USB proxy queue/latency diagnostics" OFF)

pico_sdk_init()

# ─── Pico-PIO-USB ─────────────────────────────────────────────────────────────
# Provides a software Full Speed USB host using the RP2040 PIO state machines.
# Pin the version to a known-good release; bump when upstream changes require it.
include(FetchContent)
FetchContent_Declare(
    pico_pio_usb
    GIT_REPOSITORY https://github.com/sekigon-gonnoc/Pico-PIO-USB.git
    GIT_TAG        0.7.2
)
FetchContent_MakeAvailable(pico_pio_usb)

# ─── Firmware target ─────────────────────────────────────────────────────────
add_executable(disney_infinity_proxy
    main.c
    usb_descriptors.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

target_include_directories(disney_infinity_proxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}   # picks up tusb_config.h and usb_descriptors.h
)

target_link_libraries(disney_infinity_proxy PRIVATE
    pico_stdlib
    pico_multicore
    tinyusb_device          # TinyUSB device stack (native USB hardware)
    tinyusb_host            # TinyUSB host stack (backed by Pico-PIO-USB below)
    tinyusb_board
    pico_pio_usb            # Pico-PIO-USB host controller driver
    hardware_pio
    hardware_dma
    hardware_clocks
)

target_compile_definitions(disney_infinity_proxy PRIVATE
    BOARD_TUH_RHPORT=1
    PIO_USB_USE_TINYUSB
    PROXY_ENABLE_DIAGNOSTICS=$<IF:$<BOOL:${PROXY_ENABLE_DIAGNOSTICS}>,1,0>
)

# Debug output on UART (GP0 TX / GP1 RX).
# USB CDC is disabled so the native USB port is free for the proxy function.
pico_enable_stdio_uart(disney_infinity_proxy 1)
pico_enable_stdio_usb(disney_infinity_proxy  0)

# Generate .uf2, .bin, .hex, .map
pico_add_extra_outputs(disney_infinity_proxy)
