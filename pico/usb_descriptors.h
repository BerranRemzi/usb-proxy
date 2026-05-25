#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include "tusb.h"

#define DISNEY_BCD       0x0112u   // bcdDevice 1.12 in USB BCD (each nibble is one decimal digit: 0x01=1, 0x12→1,2→.12)

// HID interrupt report size (both IN and OUT)
#define DISNEY_HID_REPORT_SIZE  32

// HID report descriptor (27 bytes, vendor-specific, no report IDs)
extern const uint8_t disney_hid_report_descriptor[];
extern const uint16_t disney_hid_report_descriptor_len;

// Runtime identity setter used to mirror upstream device VID/PID.
void usb_proxy_set_vid_pid(uint16_t vid, uint16_t pid);

#endif // USB_DESCRIPTORS_H
