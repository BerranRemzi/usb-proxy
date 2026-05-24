# Disney Infinity RP2040 USB Proxy

Transparent USB proxy for the **Xbox 360 Disney Infinity base**
(`VID 0x0E6F` / `PID 0x0129`) running on a Raspberry Pi Pico.

```
[Disney Infinity base] ──PIO-USB (GP2/GP3)──▶ [Pico] ──native USB──▶ [Xbox 360]
```

The Pico presents itself to the Xbox 360 using the exact USB descriptors of the
real base. All 32-byte interrupt IN/OUT reports are forwarded transparently.
UART logging (GP0) prints every packet for easy protocol inspection.

---

## Hardware

| Component | Notes |
|-----------|-------|
| Raspberry Pi Pico (or Pico W) | Standard Pico 1 tested |
| USB-A female breakout | For connecting the real base to the PIO-USB pins |
| USB-UART adapter (optional) | For debug output on GP0/GP1 |

### Wiring

```
Pico pin  │ Signal        │ Connects to
──────────┼───────────────┼──────────────────────────────────────────
GP0       │ UART TX       │ USB-UART RX  (debug, optional)
GP1       │ UART RX       │ USB-UART TX  (debug, optional)
GP2       │ PIO-USB D+    │ Disney Infinity base USB D+
GP3       │ PIO-USB D−    │ Disney Infinity base USB D−  ← must be GP2+1
Pin 40 (VBUS) │ +5 V     │ Disney Infinity base VBUS (via USB-A socket)
Pin 38 (GND)  │ GND       │ Disney Infinity base GND
Micro-USB │ Native USB    │ Xbox 360 USB port
```

> **Important:** GP2 and GP3 *must* be consecutive for Pico-PIO-USB.
> If you need different pins, change `PIO_USB_DP_PIN` in `main.c` — D− is
> always `PIO_USB_DP_PIN + 1`.

---

## Building

### Prerequisites

1. **Pico SDK** — clone and set `PICO_SDK_PATH`:
   ```sh
   git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
   export PICO_SDK_PATH=/path/to/pico-sdk
   ```

2. **CMake ≥ 3.13** and an **ARM GCC toolchain**:
   ```sh
   # Debian/Ubuntu
   sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential
   ```

3. An internet connection for the first build — CMake will automatically
   download **Pico-PIO-USB** via `FetchContent`.

### Configure and build

```sh
cd pico/
cmake -B build
cmake --build build -j$(nproc)
```

The firmware image is `build/disney_infinity_proxy.uf2`.

### Flash

Hold **BOOTSEL** on the Pico, connect it to your PC, then copy the `.uf2` file:

```sh
# Linux — replace sdX with your Pico mount point
cp build/disney_infinity_proxy.uf2 /media/$USER/RPI-RP2/
```

The Pico will reboot automatically and start proxying.

---

## Debug output

Connect a USB-UART adapter to GP0 (TX) and GP1 (RX) at **115 200 baud**.
Every proxied report is printed:

```
[HOST] PIO-USB host ready on GP2/GP3
[DEV ] USB device stack ready
[HOST] Device mounted — addr=1 inst=0  VID:PID=0E6F:0129
[DEV ] Xbox 360 connected
[BASE→DEV] 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ...
[DEV →360] 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ...
[XBOX→OUT] B4 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ...
[HOST→BASE] B4 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ...
```

To silence logging, set `#define LOG_ENABLED 0` in `main.c`.

---

## Disney Infinity base protocol (quick reference)

All packets are exactly 32 bytes. The first byte is the message type.

| Direction | Byte 0 | Meaning |
|-----------|--------|---------|
| Xbox → base | `0x01` | Activate portal (sent on game start) |
| Xbox → base | `0xB4` | Request NFC read from a figure slot |
| Xbox → base | `0xB5` | Request NFC write to a figure slot |
| Xbox → base | `0x83` | Set LED colour (bytes 1–3 = R, G, B) |
| Base → Xbox | `0x01` | Ready / status response |
| Base → Xbox | `0x0B` | Figure placed/removed event |
| Base → Xbox | `0xB4` | NFC read response (tag UID + data pages) |

Use the UART log to capture and inspect the full protocol while playing the game.

---

## Customising the proxy

To intercept or modify packets, edit the two functions in `main.c`:

```c
// Called when the real base sends data to the Xbox 360
void tuh_hid_report_received_cb(...)  // modify in_buf before storing

// Called when the Xbox 360 sends a command to the base
void tud_hid_set_report_cb(...)       // modify buffer before storing in out_buf
```

---

## Known limitations

| Limitation | Detail |
|------------|--------|
| Full Speed only | Pico-PIO-USB supports USB 1.1 Full Speed (12 Mbps). The Disney Infinity base is Full Speed, so this is not a problem. |
| One device at a time | `CFG_TUH_DEVICE_MAX 1` — extend if you need a hub. |
| No isochronous | Not needed for this device (interrupt endpoints only). |
