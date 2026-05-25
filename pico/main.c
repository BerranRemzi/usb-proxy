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
#include <stdint.h>

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

// Set to 1 to enable queue/latency diagnostics over UART.
#ifndef PROXY_ENABLE_DIAGNOSTICS
#define PROXY_ENABLE_DIAGNOSTICS 0
#endif
#define PROXY_DIAG_PRINT_INTERVAL_MS 1000

#define PROXY_REPORT_MAX   64
#define IN_QUEUE_DEPTH     16
#define OUT_QUEUE_DEPTH    16
#define XSM3_CONTROL_MAX   64
#define XSM3_CONTROL_TIMEOUT_MS 250

// ----------------------------------------------------------------------------
// Shared report buffers (written by one core, read by the other)
// ----------------------------------------------------------------------------

typedef struct {
    uint8_t  data[PROXY_REPORT_MAX];
    uint16_t len;
#if PROXY_ENABLE_DIAGNOSTICS
    uint32_t enqueued_us;
#endif
} in_report_t;

typedef struct {
    uint8_t  data[PROXY_REPORT_MAX];
    uint16_t len;
    uint8_t  report_id;
    uint8_t  report_type;
#if PROXY_ENABLE_DIAGNOSTICS
    uint32_t enqueued_us;
#endif
} out_report_t;

// IN direction: real base -> queue -> Xbox 360
static in_report_t in_queue[IN_QUEUE_DEPTH];
static uint8_t     in_q_head = 0;
static uint8_t     in_q_tail = 0;
static uint8_t     in_q_count = 0;
static in_report_t in_active;
static bool        in_active_valid = false;
static uint8_t     in_last[PROXY_REPORT_MAX];
static uint16_t    in_last_len = 0;
static mutex_t     in_mtx;

// OUT direction: Xbox 360 -> queue -> real base
static out_report_t out_queue[OUT_QUEUE_DEPTH];
static uint8_t      out_q_head = 0;
static uint8_t      out_q_tail = 0;
static uint8_t      out_q_count = 0;
static out_report_t out_active;
static bool         out_active_valid = false;
static bool         out_sending = false;  // true while tuh_hid_set_report is in flight
static mutex_t      out_mtx;

typedef struct {
    tusb_control_request_t setup;
    uint8_t               buffer[XSM3_CONTROL_MAX];
    uint16_t              actual_len;
    bool                  pending;
    bool                  in_flight;
    bool                  done;
    bool                  success;
} xsm3_control_state_t;

static xsm3_control_state_t xsm3_control;
static tuh_xfer_t           xsm3_host_xfer;
static tusb_control_request_t xsm3_out_request;
static uint8_t             xsm3_out_buffer[XSM3_CONTROL_MAX];
static bool                xsm3_out_request_valid = false;
static mutex_t             xsm3_mtx;

#if PROXY_ENABLE_DIAGNOSTICS
static uint32_t in_drop_count = 0;
static uint32_t out_drop_count = 0;
static uint32_t in_sent_count = 0;
static uint32_t out_sent_count = 0;
static uint32_t in_latency_sum_us = 0;
static uint32_t out_latency_sum_us = 0;
static uint32_t in_latency_max_us = 0;
static uint32_t out_latency_max_us = 0;
static uint8_t  in_q_peak = 0;
static uint8_t  out_q_peak = 0;
static uint32_t diag_last_print_ms = 0;
#endif

// ----------------------------------------------------------------------------
// Connection state (each core owns its own flag — no cross-core races)
// ----------------------------------------------------------------------------

static volatile bool device_mounted = false;  // Xbox 360 connected (core 0)
static volatile bool host_mounted   = false;  // real base connected (core 1)

// Identity of the real base on the host side
static uint8_t real_dev_addr = 0xFFu;
static uint8_t real_dev_inst = 0xFFu;
static volatile uint16_t mirrored_vid = 0;
static volatile uint16_t mirrored_pid = 0;
static volatile bool     mirrored_vid_pid_ready = false;

typedef enum {
    PROXY_DIR_BASE_TO_XBOX,
    PROXY_DIR_XBOX_TO_BASE,
} proxy_direction_t;

// ----------------------------------------------------------------------------
// Logging helpers
// ----------------------------------------------------------------------------

static const char *packet_name_for_direction(proxy_direction_t direction, uint8_t packet_type) {
    switch (packet_type) {
        case 0x01:
            return direction == PROXY_DIR_XBOX_TO_BASE ? "activate" : "status";
        case 0x0B:
            return "figure_event";
        case 0x83:
            return "set_led";
        case 0xB4:
            return direction == PROXY_DIR_XBOX_TO_BASE ? "read_figure" : "read_response";
        case 0xB5:
            return "write_figure";
        default:
            return "unknown";
    }
}

static bool xsm3_is_supported_request(tusb_control_request_t const *request) {
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    if (request->wIndex != 0x0103u) {
        return false;
    }

    switch (request->bRequest) {
        case 0x81:
            return request->bmRequestType == 0xC1u && request->wValue == 0x5B17u && request->wLength == 0x001Du;
        case 0x82:
            return request->bmRequestType == 0x41u && request->wValue == 0x0003u && request->wLength == 0x0022u;
        case 0x83:
            return request->bmRequestType == 0xC1u &&
                   (request->wValue == 0x5C28u || request->wValue == 0x5C10u) &&
                   (request->wLength == 0x002Eu || request->wLength == 0x0016u);
        case 0x87:
            return request->bmRequestType == 0x41u && request->wValue == 0x0003u && request->wLength == 0x0016u;
        default:
            return false;
    }
}

static void log_xsm3_request(const char *tag, tusb_control_request_t const *request) {
#if LOG_ENABLED
    printf("[%s] bm=%02X b=%02X wValue=%04X wIndex=%04X wLength=%u\n",
           tag,
           request->bmRequestType,
           request->bRequest,
           request->wValue,
           request->wIndex,
           request->wLength);
#else
    (void)tag;
    (void)request;
#endif
}

static void xsm3_host_control_complete_cb(tuh_xfer_t *xfer) {
    mutex_enter_blocking(&xsm3_mtx);
    xsm3_control.actual_len = (uint16_t)xfer->actual_len;
    xsm3_control.success = (xfer->result == XFER_RESULT_SUCCESS);
    xsm3_control.done = true;
    xsm3_control.pending = false;
    xsm3_control.in_flight = false;
    mutex_exit(&xsm3_mtx);
}

static bool xsm3_submit_request_and_wait(tusb_control_request_t const *request,
                                         uint8_t const *payload, uint16_t payload_len,
                                         uint16_t *actual_len) {
    if (!host_mounted || real_dev_addr == 0xFFu) {
        return false;
    }

    if (payload_len > XSM3_CONTROL_MAX) {
        return false;
    }

    mutex_enter_blocking(&xsm3_mtx);
    if (xsm3_control.pending || xsm3_control.in_flight) {
        mutex_exit(&xsm3_mtx);
        return false;
    }

    xsm3_control.setup = *request;
    xsm3_control.actual_len = 0;
    xsm3_control.pending = true;
    xsm3_control.in_flight = false;
    xsm3_control.done = false;
    xsm3_control.success = false;
    if (payload_len > 0 && payload != NULL) {
        memcpy(xsm3_control.buffer, payload, payload_len);
    }
    mutex_exit(&xsm3_mtx);

    absolute_time_t deadline = make_timeout_time_ms(XSM3_CONTROL_TIMEOUT_MS);
    while (!time_reached(deadline)) {
        bool done;
        bool success;
        uint16_t len;

        mutex_enter_blocking(&xsm3_mtx);
        done = xsm3_control.done;
        success = xsm3_control.success;
        len = xsm3_control.actual_len;
        mutex_exit(&xsm3_mtx);

        if (done) {
            if (actual_len != NULL) {
                *actual_len = len;
            }
            return success;
        }

        tight_loop_contents();
    }

    mutex_enter_blocking(&xsm3_mtx);
    xsm3_control.pending = false;
    xsm3_control.in_flight = false;
    xsm3_control.done = true;
    xsm3_control.success = false;
    mutex_exit(&xsm3_mtx);
    return false;
}

static void xsm3_host_task(void) {
    bool submit = false;

    mutex_enter_blocking(&xsm3_mtx);
    if (xsm3_control.pending && !xsm3_control.in_flight) {
        xsm3_host_xfer.daddr = real_dev_addr;
        xsm3_host_xfer.ep_addr = 0x00u;
        xsm3_host_xfer.result = XFER_RESULT_INVALID;
        xsm3_host_xfer.actual_len = 0;
        xsm3_host_xfer.setup = &xsm3_control.setup;
        xsm3_host_xfer.buffer = xsm3_control.buffer;
        xsm3_host_xfer.complete_cb = xsm3_host_control_complete_cb;
        xsm3_host_xfer.user_data = 0;
        xsm3_control.in_flight = true;
        submit = true;
    }
    mutex_exit(&xsm3_mtx);

    if (submit && !tuh_control_xfer(&xsm3_host_xfer)) {
        mutex_enter_blocking(&xsm3_mtx);
        xsm3_control.pending = false;
        xsm3_control.in_flight = false;
        xsm3_control.done = true;
        xsm3_control.success = false;
        mutex_exit(&xsm3_mtx);
    }
}

static void log_disney_report(const char *tag, proxy_direction_t direction,
                              const uint8_t *buf, uint16_t len) {
#if LOG_ENABLED
    const char *packet_name = (len > 0)
        ? packet_name_for_direction(direction, buf[0])
        : "empty";

    printf("[%s:%s]", tag, packet_name);
    for (uint16_t i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
#else
    (void)tag; (void)direction; (void)buf; (void)len;
#endif
}

static void transform_base_to_xbox_report(uint8_t *data, uint16_t *len) {
    (void)data;
    (void)len;
}

static void transform_xbox_to_base_report(uint8_t *data, uint16_t *len,
                                          uint8_t *report_id, uint8_t *report_type) {
    (void)data;
    (void)len;
    (void)report_id;
    (void)report_type;
}

static bool in_queue_push_locked(uint8_t const *data, uint16_t len) {
    if (in_q_count >= IN_QUEUE_DEPTH) {
        return false;
    }

    in_report_t *slot = &in_queue[in_q_tail];
    slot->len = len;
    memcpy(slot->data, data, len);
#if PROXY_ENABLE_DIAGNOSTICS
    slot->enqueued_us = to_us_since_boot(get_absolute_time());
#endif
    in_q_tail = (uint8_t)((in_q_tail + 1) % IN_QUEUE_DEPTH);
    in_q_count++;
#if PROXY_ENABLE_DIAGNOSTICS
    if (in_q_count > in_q_peak) {
        in_q_peak = in_q_count;
    }
#endif
    return true;
}

static bool in_queue_pop_locked(in_report_t *out) {
    if (in_q_count == 0) {
        return false;
    }

    *out = in_queue[in_q_head];
    in_q_head = (uint8_t)((in_q_head + 1) % IN_QUEUE_DEPTH);
    in_q_count--;
    return true;
}

static bool out_queue_push_locked(uint8_t const *data, uint16_t len, uint8_t report_id, uint8_t report_type) {
    if (out_q_count >= OUT_QUEUE_DEPTH) {
        return false;
    }

    out_report_t *slot = &out_queue[out_q_tail];
    slot->len = len;
    slot->report_id = report_id;
    slot->report_type = report_type;
    memcpy(slot->data, data, len);
#if PROXY_ENABLE_DIAGNOSTICS
    slot->enqueued_us = to_us_since_boot(get_absolute_time());
#endif
    out_q_tail = (uint8_t)((out_q_tail + 1) % OUT_QUEUE_DEPTH);
    out_q_count++;
#if PROXY_ENABLE_DIAGNOSTICS
    if (out_q_count > out_q_peak) {
        out_q_peak = out_q_count;
    }
#endif
    return true;
}

#if PROXY_ENABLE_DIAGNOSTICS
static void diag_print(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((now_ms - diag_last_print_ms) < PROXY_DIAG_PRINT_INTERVAL_MS) {
        return;
    }
    diag_last_print_ms = now_ms;

    uint8_t in_depth;
    uint8_t out_depth;
    bool in_active;
    bool out_active;

    mutex_enter_blocking(&in_mtx);
    in_depth = in_q_count;
    in_active = in_active_valid;
    mutex_exit(&in_mtx);

    mutex_enter_blocking(&out_mtx);
    out_depth = out_q_count;
    out_active = out_active_valid || out_sending;
    mutex_exit(&out_mtx);

    uint32_t in_avg = in_sent_count ? (in_latency_sum_us / in_sent_count) : 0;
    uint32_t out_avg = out_sent_count ? (out_latency_sum_us / out_sent_count) : 0;

    printf("[DIAG] IN q=%u peak=%u drop=%lu sent=%lu lat_avg_us=%lu lat_max_us=%lu active=%u\n",
           in_depth, in_q_peak, (unsigned long)in_drop_count, (unsigned long)in_sent_count,
           (unsigned long)in_avg, (unsigned long)in_latency_max_us, in_active ? 1u : 0u);
    printf("[DIAG] OUT q=%u peak=%u drop=%lu sent=%lu lat_avg_us=%lu lat_max_us=%lu active=%u\n",
           out_depth, out_q_peak, (unsigned long)out_drop_count, (unsigned long)out_sent_count,
           (unsigned long)out_avg, (unsigned long)out_latency_max_us, out_active ? 1u : 0u);
}
#endif

static bool out_queue_pop_locked(out_report_t *out) {
    if (out_q_count == 0) {
        return false;
    }

    *out = out_queue[out_q_head];
    out_q_head = (uint8_t)((out_q_head + 1) % OUT_QUEUE_DEPTH);
    out_q_count--;
    return true;
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
        xsm3_host_task();

        // Forward any pending OUT command to the real base.
        if (host_mounted) {
            out_report_t tx;
            bool         do_send = false;

            mutex_enter_blocking(&out_mtx);
            if (!out_active_valid) {
                out_active_valid = out_queue_pop_locked(&out_active);
            }
            if (out_active_valid && !out_sending) {
                tx = out_active;
                do_send = true;
            }
            mutex_exit(&out_mtx);

            if (do_send) {
                bool ok = tuh_hid_set_report(real_dev_addr, real_dev_inst,
                                             tx.report_id, tx.report_type,
                                             tx.data, tx.len);
                if (!ok) {
                    // Keep out_active intact; retry on next loop.
                } else {
                    mutex_enter_blocking(&out_mtx);
                    out_sending = true;
                    mutex_exit(&out_mtx);
                    log_disney_report("HOST->BASE", PROXY_DIR_XBOX_TO_BASE, tx.data, tx.len);
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

    if (!mirrored_vid_pid_ready) {
        mirrored_vid = vid;
        mirrored_pid = pid;
        mirrored_vid_pid_ready = true;
        printf("[DEV ] Mirroring VID:PID = %04X:%04X\n", vid, pid);
    }

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

        mutex_enter_blocking(&xsm3_mtx);
        memset(&xsm3_control, 0, sizeof(xsm3_control));
        xsm3_out_request_valid = false;
        mutex_exit(&xsm3_mtx);
    }
}

// Invoked when an IN report is received from the real base.
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len) {
    (void)dev_addr;
    (void)instance;

    if (len > PROXY_REPORT_MAX) {
        len = PROXY_REPORT_MAX;
    }

    uint8_t transformed[PROXY_REPORT_MAX];
    memcpy(transformed, report, len);
    transform_base_to_xbox_report(transformed, &len);
    log_disney_report("BASE->DEV", PROXY_DIR_BASE_TO_XBOX, transformed, len);

    mutex_enter_blocking(&in_mtx);
    memcpy(in_last, transformed, len);
    in_last_len = len;
    bool queued = in_queue_push_locked(transformed, len);
    mutex_exit(&in_mtx);

#if LOG_ENABLED
    if (!queued) {
        printf("[WARN] IN queue full, dropping report\n");
    }
#endif

#if PROXY_ENABLE_DIAGNOSTICS
    if (!queued) {
        in_drop_count++;
    }
#endif

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

#if PROXY_ENABLE_DIAGNOSTICS
    uint32_t latency_us = to_us_since_boot(get_absolute_time()) - out_active.enqueued_us;
    out_sent_count++;
    out_latency_sum_us += latency_us;
    if (latency_us > out_latency_max_us) {
        out_latency_max_us = latency_us;
    }
#endif

    mutex_enter_blocking(&out_mtx);
    out_sending = false;
    out_active_valid = false;
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

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (!xsm3_is_supported_request(request)) {
        return false;
    }

    if (stage == CONTROL_STAGE_SETUP) {
        log_xsm3_request("XSM3 SETUP", request);

        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            uint16_t actual_len = 0;
            if (!xsm3_submit_request_and_wait(request, NULL, 0, &actual_len)) {
                return false;
            }

            if (actual_len > request->wLength) {
                actual_len = request->wLength;
            }

            return tud_control_xfer(rhport, request, xsm3_control.buffer, actual_len);
        }

        xsm3_out_request = *request;
        xsm3_out_request_valid = true;
        return tud_control_xfer(rhport, request, xsm3_out_buffer, request->wLength);
    }

    if (stage == CONTROL_STAGE_DATA) {
        if (request->bmRequestType_bit.direction == TUSB_DIR_OUT && xsm3_out_request_valid) {
            return xsm3_submit_request_and_wait(&xsm3_out_request,
                                                xsm3_out_buffer,
                                                xsm3_out_request.wLength,
                                                NULL);
        }
        return true;
    }

    if (stage == CONTROL_STAGE_ACK) {
        if (request->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            xsm3_out_request_valid = false;
        }
        return true;
    }

    return false;
}

// Invoked when the Xbox 360 issues a GET_REPORT control request.
// Return the most recent IN report received from the real base.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                 hid_report_type_t report_type,
                                 uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;

    mutex_enter_blocking(&in_mtx);
    uint16_t len = in_last_len;
    if (len > reqlen) {
        len = reqlen;
    }
    memcpy(buffer, in_last, len);
    mutex_exit(&in_mtx);

    return len;
}

// Invoked when the Xbox 360 sends an OUT report (interrupt OUT or SET_REPORT).
// Buffer this for core 1 to forward to the real base.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                             hid_report_type_t report_type,
                             uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;

    if (bufsize > PROXY_REPORT_MAX) {
        bufsize = PROXY_REPORT_MAX;
    }

    uint8_t transformed[PROXY_REPORT_MAX];
    memcpy(transformed, buffer, bufsize);
    uint8_t mutable_report_id = report_id;
    uint8_t mutable_report_type = (uint8_t)report_type;
    transform_xbox_to_base_report(transformed, &bufsize,
                                  &mutable_report_id, &mutable_report_type);
    log_disney_report("XBOX->OUT", PROXY_DIR_XBOX_TO_BASE, transformed, bufsize);

    mutex_enter_blocking(&out_mtx);
    bool queued = out_queue_push_locked(transformed, bufsize,
                                        mutable_report_id, mutable_report_type);
    mutex_exit(&out_mtx);

#if LOG_ENABLED
    if (!queued) {
        printf("[WARN] OUT queue full, dropping report\n");
    }
#endif

#if PROXY_ENABLE_DIAGNOSTICS
    if (!queued) {
        out_drop_count++;
    }
#endif
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
    mutex_init(&xsm3_mtx);

    // Start core 1 (USB host) before core 0 initialises the device stack so
    // the PIO programs load before TinyUSB allocates DMA channels.
    multicore_launch_core1(core1_main);

    printf("[DEV ] Waiting for upstream HID VID:PID before USB device init...\n");
    while (!mirrored_vid_pid_ready) {
        sleep_ms(1);
    }

    usb_proxy_set_vid_pid((uint16_t)mirrored_vid, (uint16_t)mirrored_pid);

    // Initialise TinyUSB device stack on rhport 0 (native USB hardware).
    tud_init(0);
    printf("[DEV ] USB device stack ready\n");

    while (true) {
        tud_task();

#if PROXY_ENABLE_DIAGNOSTICS
        diag_print();
#endif

        // Forward any pending IN report to the Xbox 360.
        if (device_mounted && tud_hid_ready()) {
            in_report_t tx;
            bool        do_send = false;

            mutex_enter_blocking(&in_mtx);
            if (!in_active_valid) {
                in_active_valid = in_queue_pop_locked(&in_active);
            }
            if (in_active_valid) {
                tx = in_active;
                do_send = true;
            }
            mutex_exit(&in_mtx);

            if (do_send) {
                // report_id = 0: no report ID prefix in the 32-byte packet
                if (tud_hid_report(0, tx.data, tx.len)) {
#if PROXY_ENABLE_DIAGNOSTICS
                    uint32_t latency_us = to_us_since_boot(get_absolute_time()) - tx.enqueued_us;
                    in_sent_count++;
                    in_latency_sum_us += latency_us;
                    if (latency_us > in_latency_max_us) {
                        in_latency_max_us = latency_us;
                    }
#endif
                    mutex_enter_blocking(&in_mtx);
                    in_active_valid = false;
                    mutex_exit(&in_mtx);
                    log_disney_report("DEV->360", PROXY_DIR_BASE_TO_XBOX, tx.data, tx.len);
                }
            }
        }
    }
}
