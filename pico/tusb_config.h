#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Common
// ----------------------------------------------------------------------------
#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUSB_DEBUG        0

// ----------------------------------------------------------------------------
// USB Device — rhport 0 (native RP2040 USB hardware)
// Presents as a Disney Infinity base to the Xbox 360.
// ----------------------------------------------------------------------------
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE 64

// One HID interface (vendor-specific, 32-byte IN + OUT reports)
#define CFG_TUD_HID            1
#define CFG_TUD_HID_EP_BUFSIZE 64

// Unused device classes
#define CFG_TUD_CDC            0
#define CFG_TUD_MSC            0
#define CFG_TUD_MIDI           0
#define CFG_TUD_VENDOR         0

// ----------------------------------------------------------------------------
// USB Host — rhport 1 (Pico-PIO-USB on configurable GPIO pins)
// Enumerates and communicates with the real Disney Infinity base.
// ----------------------------------------------------------------------------
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1   // use Pico-PIO-USB as the host controller
#define CFG_TUH_DEVICE_MAX      1
#define CFG_TUH_HID             4   // support up to 4 HID instances per device
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
