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

pico_sdk_init()

# ─── Pico-PIO-USB ─────────────────────────────────────────────────────────────
# Provides a software Full Speed USB host using the RP2040 PIO state machines.
# Pin the version to a known-good release; bump when upstream changes require it.
include(FetchContent)
FetchContent_Declare(
    pico_pio_usb
    GIT_REPOSITORY https://github.com/sekigon-gonnoc/Pico-PIO-USB.git
    GIT_TAG        0.6.0
)
FetchContent_MakeAvailable(pico_pio_usb)

# ─── Firmware target ─────────────────────────────────────────────────────────
add_executable(disney_infinity_proxy
    main.c
    usb_descriptors.c
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

# Debug output on UART (GP0 TX / GP1 RX).
# USB CDC is disabled so the native USB port is free for the proxy function.
pico_enable_stdio_uart(disney_infinity_proxy 1)
pico_enable_stdio_usb(disney_infinity_proxy  0)

# Generate .uf2, .bin, .hex, .map
pico_add_extra_outputs(disney_infinity_proxy)
