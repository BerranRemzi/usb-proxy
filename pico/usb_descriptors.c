#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

// ----------------------------------------------------------------------------
// HID Report Descriptor  (27 bytes)
//
// Vendor-specific: 32-byte IN (base → Xbox) and 32-byte OUT (Xbox → base).
// No report IDs — the full 32 bytes are the report.
// wDescriptorLength in the real device is 27, matching this exactly.
// ----------------------------------------------------------------------------
const uint8_t disney_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor-Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor 0x01)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x02,        //   Usage (Vendor 0x02) — Input
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x20,        //   Report Count (32)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x09, 0x03,        //   Usage (Vendor 0x03) — Output
    0x91, 0x02,        //   Output (Data, Variable, Absolute)
    0xC0               // End Collection
};

const uint16_t disney_hid_report_descriptor_len =
    sizeof(disney_hid_report_descriptor);

// ----------------------------------------------------------------------------
// Device Descriptor
// Mirrors the real Disney Infinity base exactly so the Xbox 360 accepts it.
// ----------------------------------------------------------------------------
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,        // defined at interface level
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = DISNEY_VID,
    .idProduct          = DISNEY_PID,
    .bcdDevice          = DISNEY_BCD,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01,
};

// ----------------------------------------------------------------------------
// Configuration Descriptor
//
// One interface, HID class (0x03), no boot subclass/protocol, 2 endpoints:
//   EP1 IN  0x81 — Interrupt, 32 bytes, 1 ms
//   EP1 OUT 0x01 — Interrupt, 32 bytes, 1 ms
//
// wTotalLength = TUD_CONFIG_DESC_LEN(9) + TUD_HID_INOUT_DESC_LEN(32) = 41 (0x0029)
// This matches the value captured from the real device.
// ----------------------------------------------------------------------------
#define EP_HID_IN   0x81u
#define EP_HID_OUT  0x01u
#define EP_INTERVAL 1u          // 1 ms polling interval

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t desc_configuration[] = {
    // Configuration header
    TUD_CONFIG_DESCRIPTOR(1 /*config num*/, 1 /*num interfaces*/, 0 /*iConfig*/,
                          CONFIG_TOTAL_LEN, 0 /*bmAttributes: bus-powered*/, 500),

    // HID interface with EP1 OUT + EP1 IN
    TUD_HID_INOUT_DESCRIPTOR(0 /*itf num*/, 0 /*iInterface*/,
                             HID_ITF_PROTOCOL_NONE,
                             sizeof(disney_hid_report_descriptor),
                             EP_HID_OUT, EP_HID_IN,
                             DISNEY_HID_REPORT_SIZE, EP_INTERVAL),
};

// ----------------------------------------------------------------------------
// String Descriptors
// ----------------------------------------------------------------------------
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},        // 0: Language ID — English (0x0409)
    "Performance Designed Products",   // 1: Manufacturer
    "Disney Infinity Base",            // 2: Product
};

// ----------------------------------------------------------------------------
// TinyUSB device-stack callbacks
// ----------------------------------------------------------------------------

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return disney_hid_report_descriptor;
}

// Reusable UTF-16LE string buffer
static uint16_t _desc_str[64];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        // Language ID descriptor: copy the two raw bytes (0x09, 0x04)
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 63u) {
            chr_count = 63;
        }
        // ASCII → UTF-16LE
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1u + i] = (uint16_t)str[i];
        }
    }

    // Header: [length in bytes | descriptor type]
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return _desc_str;
}
