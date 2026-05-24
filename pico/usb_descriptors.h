#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

// ----------------------------------------------------------------------------
// Disney Infinity base — Xbox 360 version
//   Vendor:  0x0E6F  Performance Designed Products
//   Product: 0x0129  Disney Infinity Base
// ----------------------------------------------------------------------------
#define DISNEY_VID       0x0E6Fu
#define DISNEY_PID       0x0129u
#define DISNEY_BCD       0x0112u   // bcdDevice 1.12 in USB BCD (each nibble is one decimal digit: 0x01=1, 0x12→1,2→.12)

// HID interrupt report size (both IN and OUT)
#define HID_REPORT_SIZE  32

// HID report descriptor (27 bytes, vendor-specific, no report IDs)
extern const uint8_t disney_hid_report_descriptor[];
extern const uint16_t disney_hid_report_descriptor_len;

#endif // USB_DESCRIPTORS_H
