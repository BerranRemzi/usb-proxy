/*
 * disney_infinity_proxy — main.c
 *
 * Transparent USB proxy for the Xbox 360 Disney Infinity base on RP2040.
 *
 * Architecture
 * ─────────────
 *  Core 0 │ TinyUSB device stack (native USB hardware, rhport 0)
 *         │ Appears to the Xbox 360 as a genuine Disney Infinity base.
 *         │ Forwards IN reports from the real base to the Xbox 360.
 *
 *  Core 1 │ TinyUSB host stack (Pico-PIO-USB, rhport 1)
 *         │ Enumerates and polls the real Disney Infinity base.
 *         │ Forwards OUT commands received from the Xbox 360.
 *
 * Shared state between cores is protected by lightweight pico mutexes.
 * UART (GP0 TX / GP1 RX, 115 200 baud) is used for debug logging so the
 * native USB port is free for the proxy.
 *
 * Pin connections
 * ───────────────
 *  GP0  → UART TX  (connect to USB-UART adapter for debug)
 *  GP1  ← UART RX
 *  GP2  → PIO-USB D+  │  To the real Disney Infinity base via USB-A socket.
 *  GP3  → PIO-USB D−  │  D− must be GP2+1; do NOT swap.
 *  VBUS → 5 V supply for the USB-A socket (e.g. pin 40 on Pico)
 *  USB micro/type-C port → Xbox 360 USB port
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include "pio_usb.h"

#include "usb_descriptors.h"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

// PIO-USB D+ pin. D− is automatically assigned to PIO_USB_DP_PIN + 1.
#define PIO_USB_DP_PIN   2

// Set to 0 to silence all UART log output (e.g. for production builds).
#define LOG_ENABLED      1

// ----------------------------------------------------------------------------
// Shared report buffers (written by one core, read by the other)
// ----------------------------------------------------------------------------

// IN direction:  real base → core 1 callback → in_buf → core 0 → Xbox 360
static uint8_t  in_buf[DISNEY_HID_REPORT_SIZE];
static bool     in_ready = false;       // protected by in_mtx
static mutex_t  in_mtx;

// OUT direction: Xbox 360 → core 0 callback → out_buf → core 1 → real base
static uint8_t  out_buf[DISNEY_HID_REPORT_SIZE];
static bool     out_ready   = false;    // protected by out_mtx
static bool     out_sending = false;    // true while tuh_hid_set_report is in flight
static mutex_t  out_mtx;

// ----------------------------------------------------------------------------
// Connection state (each core owns its own flag — no cross-core races)
// ----------------------------------------------------------------------------

static volatile bool device_mounted = false;  // Xbox 360 connected (core 0)
static volatile bool host_mounted   = false;  // real base connected (core 1)

// Identity of the real base on the host side
static uint8_t real_dev_addr = 0xFFu;
static uint8_t real_dev_inst = 0xFFu;

// ----------------------------------------------------------------------------
// Logging helpers
// ----------------------------------------------------------------------------

static void log_report(const char *tag, const uint8_t *buf, uint8_t len) {
#if LOG_ENABLED
    printf("[%s]", tag);
    for (uint8_t i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
#else
    (void)tag; (void)buf; (void)len;
#endif
}

// ----------------------------------------------------------------------------
// Core 1 — USB host task (Pico-PIO-USB)
// ----------------------------------------------------------------------------

static void core1_main(void) {
    // Brief delay so core 0 finishes its USB device initialisation first.
    sleep_ms(10);

    // Configure Pico-PIO-USB as the host controller on rhport 1.
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIO_USB_DP_PIN;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(1);

    printf("[HOST] PIO-USB host ready on GP%d/GP%d\n",
           PIO_USB_DP_PIN, PIO_USB_DP_PIN + 1);

    while (true) {
        tuh_task();

        // Forward any pending OUT command to the real base.
        if (host_mounted) {
            uint8_t buf[DISNEY_HID_REPORT_SIZE];
            bool    do_send = false;

            mutex_enter_blocking(&out_mtx);
            if (out_ready && !out_sending) {
                memcpy(buf, out_buf, DISNEY_HID_REPORT_SIZE);
                out_ready   = false;
                out_sending = true;
                do_send     = true;
            }
            mutex_exit(&out_mtx);

            if (do_send) {
                bool ok = tuh_hid_set_report(real_dev_addr, real_dev_inst,
                                             0, HID_REPORT_TYPE_OUTPUT,
                                             buf, DISNEY_HID_REPORT_SIZE);
                if (!ok) {
                    // Endpoint busy; clear sending flag so we retry next cycle.
                    mutex_enter_blocking(&out_mtx);
                    out_sending = false;
                    mutex_exit(&out_mtx);
                } else {
                    log_report("HOST→BASE", buf, DISNEY_HID_REPORT_SIZE);
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// TinyUSB host callbacks  (called from tuh_task() on core 1)
// ----------------------------------------------------------------------------

// Invoked when a HID interface is mounted on the host side.
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("[HOST] Device mounted — addr=%u inst=%u  VID:PID=%04X:%04X\n",
           dev_addr, instance, vid, pid);

    real_dev_addr = dev_addr;
    real_dev_inst = instance;
    host_mounted  = true;

    // Start polling IN reports from the real base.
    tuh_hid_receive_report(dev_addr, instance);
}

// Invoked when a HID interface is unmounted from the host side.
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("[HOST] Device unmounted — addr=%u inst=%u\n", dev_addr, instance);
    if (real_dev_addr == dev_addr && real_dev_inst == instance) {
        host_mounted   = false;
        real_dev_addr  = 0xFFu;
        real_dev_inst  = 0xFFu;
    }
}

// Invoked when an IN report is received from the real base.
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len) {
    if (len > DISNEY_HID_REPORT_SIZE) {
        len = DISNEY_HID_REPORT_SIZE;
    }
    log_report("BASE→DEV", report, (uint8_t)len);

    mutex_enter_blocking(&in_mtx);
    memcpy(in_buf, report, len);
    in_ready = true;
    mutex_exit(&in_mtx);

    // Re-arm: keep polling the real base.
    tuh_hid_receive_report(dev_addr, instance);
}

// Invoked when a SET_REPORT to the real base completes.
void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                     uint8_t report_id,
                                     uint8_t report_type,
                                     uint16_t len) {
    (void)dev_addr; (void)instance; (void)report_id;
    (void)report_type; (void)len;

    mutex_enter_blocking(&out_mtx);
    out_sending = false;
    mutex_exit(&out_mtx);
}

// ----------------------------------------------------------------------------
// TinyUSB device callbacks  (called from tud_task() on core 0)
// ----------------------------------------------------------------------------

// Invoked when the Xbox 360 mounts the proxy.
void tud_mount_cb(void) {
    printf("[DEV ] Xbox 360 connected\n");
    device_mounted = true;
}

// Invoked when the Xbox 360 disconnects.
void tud_umount_cb(void) {
    printf("[DEV ] Xbox 360 disconnected\n");
    device_mounted = false;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {
}

// Invoked when the Xbox 360 issues a GET_REPORT control request.
// Return the most recent IN report received from the real base.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                 hid_report_type_t report_type,
                                 uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;

    uint16_t len = (reqlen < DISNEY_HID_REPORT_SIZE) ? reqlen : DISNEY_HID_REPORT_SIZE;

    mutex_enter_blocking(&in_mtx);
    memcpy(buffer, in_buf, len);
    mutex_exit(&in_mtx);

    return len;
}

// Invoked when the Xbox 360 sends an OUT report (interrupt OUT or SET_REPORT).
// Buffer this for core 1 to forward to the real base.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                             hid_report_type_t report_type,
                             uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;

    if (bufsize > DISNEY_HID_REPORT_SIZE) {
        bufsize = DISNEY_HID_REPORT_SIZE;
    }
    log_report("XBOX→OUT", buffer, (uint8_t)bufsize);

    mutex_enter_blocking(&out_mtx);
    memcpy(out_buf, buffer, bufsize);
    out_ready = true;
    mutex_exit(&out_mtx);
}

// ----------------------------------------------------------------------------
// Core 0 — main / USB device task
// ----------------------------------------------------------------------------

int main(void) {
    // PIO-USB requires 120 MHz for correct Full Speed (12 Mbps) timing.
    set_sys_clock_khz(120000, true);

    stdio_init_all();   // UART only (USB CDC disabled in CMakeLists)
    printf("\n=== Disney Infinity RP2040 USB Proxy ===\n");
    printf("    PIO-USB host: GP%d (D+) / GP%d (D-)\n",
           PIO_USB_DP_PIN, PIO_USB_DP_PIN + 1);
    printf("    Device port : native USB\n\n");

    mutex_init(&in_mtx);
    mutex_init(&out_mtx);

    // Start core 1 (USB host) before core 0 initialises the device stack so
    // the PIO programs load before TinyUSB allocates DMA channels.
    multicore_launch_core1(core1_main);

    // Initialise TinyUSB device stack on rhport 0 (native USB hardware).
    tud_init(0);
    printf("[DEV ] USB device stack ready\n");

    while (true) {
        tud_task();

        // Forward any pending IN report to the Xbox 360.
        if (device_mounted && tud_hid_ready()) {
            uint8_t buf[DISNEY_HID_REPORT_SIZE];
            bool    do_send = false;

            mutex_enter_blocking(&in_mtx);
            if (in_ready) {
                memcpy(buf, in_buf, DISNEY_HID_REPORT_SIZE);
                in_ready = false;
                do_send  = true;
            }
            mutex_exit(&in_mtx);

            if (do_send) {
                // report_id = 0: no report ID prefix in the 32-byte packet
                tud_hid_report(0, buf, DISNEY_HID_REPORT_SIZE);
                log_report("DEV →360", buf, DISNEY_HID_REPORT_SIZE);
            }
        }
    }
}
