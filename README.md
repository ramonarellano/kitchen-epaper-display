# Kitchen e-Paper Display Firmware

[Original PhotoPainter Repository](https://github.com/waveshareteam/PhotoPainter)

## Overview

This project provides the Raspberry Pi Pico (RP2040) firmware that requests a panel-ready raw image from the ESP32 over UART and renders it on a Waveshare 7.3 inch 7-color e-paper display.

Current live protocol:

1. Pico sends `SENDIMG\n` on UART1.
2. ESP32 replies with `ACK\n` twice.
3. ESP32 sends SOF `0xAA 0x55 0xAA 0x55`.
4. ESP32 sends a 4-byte big-endian payload length.
5. ESP32 streams 192000 raw bytes.
6. Pico validates the framed payload and updates the panel.

UART details:

- UART: `uart1`
- Baud rate: `115200`
- Pico TX/RX pins: GPIO4/GPIO5
- Image buffer size: `192000` bytes

## Current Diagnostic Build

- Firmware tag: `POWER_CYCLE_v1`
- **One-shot mode:** Pico boots, requests one image, displays it, signals `PICODONE\n`, then halts
- ESP32 controls Pico power via GPIO 25 + MOSFET (see ESP32 README)
- Per-cycle `FULL_REINIT` (hardware RST + full register reload)
- Explicit `POWER_ON (0x04)` before `DISPLAY_REFRESH (0x12)`
- `EPD_7IN3F_Sleep()` after display: `POWER_OFF (0x02)` + `DEEP_SLEEP (0x07 0xA5)` + 2000ms settle
- Up to 3 image request retries on timeout
- Final PLOG flush before `PICODONE` so ESP32 captures all diagnostics

## Main Features

- Robust framed UART receive path with ACK, SOF, size-header, and payload validation.
- Per-cycle display re-init with retry logic for `Init()` and `PowerOn()` timeouts.
- Split BUSY-pin instrumentation around `POWER_ON (0x04)` and `DISPLAY_REFRESH (0x12)`.
- Remote Pico logging (PLOG) flushed to the ESP32 before each `SENDIMG`.
- USB serial diagnostics and onboard LED status patterns.
- Small host-side ACK parser test in `tests/test_ack.c`.

## Key Files

- `main.c` - Pico one-shot entry point, UART protocol, retries, and PLOG
- `lib/e-Paper/EPD_7in3f.c` - display driver and BUSY instrumentation
- `lib/e-Paper/EPD_7in3f.h` - exported driver interfaces and diagnostic globals
- `tests/test_ack.c` - host-side ACK detection test

## Configuration

The main tunables live at the top of `main.c`.

- `MAX_IMAGE_RETRIES` - currently `3` (retry attempts for image request)
- `ACK_TIMEOUT_MS` - currently `10000`
- `SOF_TIMEOUT_MS` - currently `60000`
- `DATA_TIMEOUT_MS` - currently `180000`
- `RETRY_WAIT_MS` - currently `30000`
- `POST_SEND_DELAY_MS` - currently `20`
- `ACK_BUFFER_SIZE` - currently `64`
- `PICO_UART_LOGGING` - set to `0` to disable remote logging

## Remote Logging (PLOG)

The Pico buffers diagnostic log lines locally and flushes them to the ESP32 over UART immediately before the next `SENDIMG`. The ESP32 stores them in SPIFFS with a `[PICO]` prefix.

Common events in the current firmware:

- `BOOT vbus=X fw=POWER_CYCLE_v1`
- `SENDIMG_START attempt=X`
- `SENDIMG_RESULT rc=X recv=Y attempt=Z`
- `DISPLAY chk=0 bytes=Y first4=Z`
- `FULL_REINIT`
- `REINIT_DONE busy_before=X busy_after_rst=Y rc=Z attempt=Z`
- `POWER_ON_PRE rc=X busy=A->B attempt=Y`
- `DISPLAY_DONE ms=X forced=Y rc=Z`
- `EPD_PHASES pwr_on=X refresh=Y pwr_off=Z`
- `EPD_BUSY04 A->B`
- `EPD_BUSY12 C->D`
- `REFRESH_VERDICT real=0/1 refresh_ms=X disp_rc=Y`
- `EPD_SLEEP rc=X`
- `RECV_TIMEOUT attempt=X`
- `RECV_FAIL rc=X attempts=N`

Notes:

- In one-shot mode, there are no `CYCLE`, `WAIT_START`, `WAIT_TICK`, or `WAIT_DONE` events.
- `PICODONE\n` is sent on UART after the final PLOG flush (not logged as PLOG — it's a control signal).

## Display Constraints

These constraints are established by the current analysis and external research (see `summary/ai-report.md` and `ANALYSIS.md`).

1. `EPD_7IN3F_Sleep()` must be called after every display cycle.
  - The canonical Waveshare lifecycle is: `Init → display → POWER_OFF → DEEP_SLEEP → RST → Init`.
  - Skipping `DEEP_SLEEP (0x07)` leaves the controller's analog rails in a half-de-energized state that causes progressive booster/VCOM failures.
  - Only a hardware RST can wake the controller from deep sleep.

2. `POWER_OFF (0x02)` must still be sent after each refresh.
  - This protects the panel from being left powered.

3. Hardware RST must use sufficient timing to wake from deep sleep.
  - Current: 50ms low + 300ms settle (well above the 10ms minimum).

4. A healthy 7-color refresh takes about 31 seconds.
  - Healthy cycles look like `DISPLAY_DONE ms=31197..31207` and `refresh=30600..30610`.
  - Bogus cycles look like `DISPLAY_DONE ms=2457` and `refresh=2010`.

## Build And Flash

Preferred:

```sh
./build_and_flash.sh
```

Manual build:

```sh
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Then flash the UF2:

```sh
cp epd.uf2 /Volumes/RPI-RP2/
```

## Host-Side Test

There is a small host-buildable test that validates ACK detection.

```sh
gcc tests/test_ack.c -o tests/test_ack
./tests/test_ack
```

Expected output:

```text
All contains_ack tests passed
```

## Current Debugging Focus

The active investigation is Bug #15: the panel can refresh correctly for several cycles and then stop performing a real physical refresh even though image transfer still succeeds.

Useful signatures:

- Healthy cycle:
  - `POWER_ON_PRE rc=0 busy=1->0`
  - `DISPLAY_DONE ms=31197..31207 rc=0`
  - `EPD_PHASES pwr_on=0 refresh=30600..30610 pwr_off=150`
  - `EPD_BUSY04 1->0`

- Failure mode A:
  - `POWER_ON_PRE rc=0 busy=1->1`
  - `DISPLAY_DONE ms=2457 rc=-2`
  - `EPD_PHASES pwr_on=0 refresh=2010 pwr_off=0`
  - `EPD_BUSY04 1->1`

- Failure mode B:
  - `POWER_ON_PRE rc=0 busy=1->0`
  - `DISPLAY_DONE ms=2457 rc=-2`
  - `EPD_PHASES pwr_on=0 refresh=2010 pwr_off=0`
  - `EPD_BUSY04 1->0`