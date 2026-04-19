# Kitchen e-Paper Display Firmware

[Original PhotoPainter Repository](https://github.com/waveshareteam/PhotoPainter)

## Overview
This project provides firmware for a Raspberry Pi Pico (RP2040) that receives images from an ESP32 over UART and displays them on a Waveshare 7.3" e-Paper display. The firmware provides robust status feedback via onboard LEDs and logs messages to both UART and USB serial.

## Main Features

- **UART Handshake with ESP32:**
  - Uses UART1 (GPIO 4/5) for both handshake and image transfer: Pico sends `HELLO ESP32`, waits for `HELLO RP2040` reply, then requests image data over the same connection.
  - Retries handshake up to 5 times, then displays a fallback image if unsuccessful.
  - Continues retrying handshake in the background until it succeeds, then proceeds to image transfer.

- **Image Transfer via UART:**
  - Uses UART1 (GPIO 4/5) for image transfer.
  - Pico requests an image from the ESP32 by sending `SENDIMG` over UART1.
  - Waits for image data (with a 5-second timeout).
  # Kitchen e-Paper Display Firmware

  [Original PhotoPainter Repository](https://github.com/waveshareteam/PhotoPainter)

  ## Overview

  This project provides firmware for a Raspberry Pi Pico (RP2040) that receives images from an ESP32 over UART and displays them on a Waveshare 7.3" e-Paper display. The firmware provides robust status feedback via onboard LEDs and logs messages to both UART and USB serial.

  ## Recent changes (refactor & debugging)

  - **Fixed: display only refreshing once after power-on.** `EPD_7IN3F_Sleep()` put the panel into deep sleep where the BUSY pin floats HIGH. On subsequent `Init()` calls, `ReadBusyH()` returned instantly, causing `Display()` to complete in ~447ms (SPI transfer only) with no physical refresh. The fix removes the deep-sleep call entirely; `TurnOnDisplay()` already sends POWER_OFF (0x02) which keeps the panel in low-power standby (~50µA) that `Init()` can wake from reliably.
  - **Fixed: display only refreshing on the first cycle (Bug #15).** After POWER_OFF (0x02), calling `Init()` + hardware `Reset()` corrupts the BUSY pin state — subsequent `ReadBusyH()` calls return in 0ms, producing 447ms bogus refreshes with `pwr_on=0 refresh=0 pwr_off=0`. The fix moves `Init()` to boot only (called once), matching the Waveshare demo pattern. POWER_OFF preserves register config; `TurnOnDisplay()` re-powers on each cycle with POWER_ON (0x04).
  - **Fix: per-cycle soft re-init (Bug #15 reopened, fw=SOFT_REINIT_v1).** Init-once doesn't survive 60-minute POWER_OFF idle — panel SRAM registers decay at <0.01µA standby. New `EPD_7IN3F_ReloadConfig()` re-sends all register commands before each display cycle WITHOUT hardware reset, matching the GxEPD2 `_InitDisplay()` pattern.
  - **Fix: full hardware re-init + PowerOn per cycle (fw=FULL_REINIT_v1).** Soft re-init failed — controller ignores SPI after 60-min POWER_OFF. Now does full `Init()` (hardware reset with 50ms/300ms timing) + explicit `PowerOn()` before data writes, matching GxEPD2's exact sequence. Partially successful: 4 healthy refreshes before BUSY stuck.
  - **Fix: abort-on-timeout + retry + WaitBusyTransition (fw=FULL_REINIT_v3).** `WaitBusyTransition` (require LOW→HIGH) used only for DISPLAY_REFRESH. `ReadBusyH` used for Init (after Reset), PowerOn, and POWER_OFF. TurnOnDisplay no longer sends redundant POWER_ON (caller's PowerOn() handles it). Retry up to 3 attempts on Init/PowerOn failure.
  - Centralized configuration at the top of `main.c` (all timeouts and buffer sizes in one place).
  - Robust ACK detection and ACK timeout increased to 10s; SOF and data-timeout handling improved.
  - Removed fallback-image display (simplified flow) and made timeouts wait-and-retry in a consistent way.
  - Added a small host-test (`tests/test_ack.c`) that validates ACK substring detection.
  - Added EPD phase-level timing (`EPD_PHASES pwr_on=X refresh=Y pwr_off=Z`) and `REFRESH_VERDICT real=0/1` to diagnose whether a display refresh actually occurred.
  - BUSY pin state is now sampled before `Init()` and logged as `busy_before=X` to detect floating-HIGH conditions.
  - PLOG heartbeats (`WAIT_TICK`) logged every 10 minutes during the 60-minute idle wait to confirm the Pico survived.

  ## Main Features

  - UART handshake and image request/transfer between Pico (UART1, GPIO4/5) and ESP32.
  - Robust framing: the receiver looks for a Start-Of-Frame (SOF), reads a 4-byte length header, then reads the payload with timeouts and logging.
  - LED status patterns and USB serial logging for diagnostics.

  ## Configuration (quick reference)

  Edit `main.c` to tune these values at the top of the file (or change via your build system):

  - `UART_BAUD` — UART baud rate (default 115200)
  - `ACK_TIMEOUT_MS` — how long to wait for ACK (default 10000 ms)
  - `SOF_TIMEOUT_MS` — wait for SOF (default 60000 ms)
  - `DATA_TIMEOUT_MS` — overall image-data timeout (default 180000 ms)
  - `RETRY_WAIT_MS` — how long to wait after a timeout before retrying (default 30000 ms)
  - `POST_SEND_DELAY_MS` — small delay after sending `SENDIMG` (default 20 ms)
  - `ACK_BUFFER_SIZE` — temporary buffer for incoming ACK lines

  ## Remote Logging (PLOG)

  The Pico can send diagnostic messages to the ESP32 over UART, which stores them in its persistent SPIFFS log. This allows full Pico-side visibility without a USB connection.

  - Controlled by the `PICO_UART_LOGGING` flag at the top of `main.c` (set to `0` to disable).
  - Messages are buffered during Pico processing and flushed to the ESP32 right before each `SENDIMG`, when the ESP32 is awake and listening.
  - Events logged: `BOOT vbus=X fw=INIT_ONCE_v1`, `EPD_INIT_BOOT`, `EPD_INIT_BOOT_DONE busy_before=X`, `SENDIMG_START`, `SENDIMG_RESULT rc=X recv=Y`, `DISPLAY chk=X bytes=Y`, `DISPLAY_DONE ms=X forced=Y`, `EPD_PHASES pwr_on=X refresh=Y pwr_off=Z`, `REFRESH_VERDICT real=0/1 refresh_ms=X`, `STANDBY last_sum=0`, `WAIT_START`, `WAIT_TICK min_left=X`, `WAIT_DONE`, `SKIP chk=X last=Y`, `RECV_TIMEOUT retry attempts=N`, `RECV_FAIL rc=X attempts=N`.
  - Retrieve logs from the ESP32 using `python3 fetch_and_clear_logs.py` in the ESP32 repo.

  ## Running the unit tests (host)

  There is a small host-buildable test to validate the ACK-detection logic.

  Build and run:

  ```bash
  gcc tests/test_ack.c -o tests/test_ack
  ./tests/test_ack
  ```

  Expected output: `All contains_ack tests passed`.

  ## Debugging the display update issue

  If the log prints `Image received` and `EPD_7IN3F_Display() done` but the panel doesn't visibly update after the first run, try the following:

  1. Make sure `DISABLE_DISPLAY_SLEEP` is not forcing deep-sleep too early. The code now waits a short time (5s) after `EPD_7IN3F_Display()` before sleeping; you can skip sleeping by defining `DISABLE_DISPLAY_SLEEP`.
  2. Verify `img_size` matches the expected buffer size (driver's WidthBytes × Height). The driver uses 4-bit-per-pixel packing (two pixels per byte) — confirm the ESP32 produces the same packing.
  3. Try showing a forced test pattern (all 0xFF or all 0x00) and see whether the display changes. If it does, the issue is likely pixel packing or order.
  4. If updates work when skipping sleep but not otherwise, increase the delay or keep sleep disabled until you confirm the proper refresh sequence.

  ## Recommended framing and sender example (ESP32)

  Use a binary framing that both sides agree on. The current examples in the repo use a SOF, a 4-byte length, and raw payload. See `README` above for an Arduino-style sender stub.

  ## Building and flashing

  Follow the existing steps in `build_and_flash.sh`. The project expects a Pico SDK environment for full cross-compilation. For host-side testing use the small tests in `tests/`.

  ## Troubleshooting tips

  - Enable verbose USB-serial logs to capture the checksum and first bytes of the incoming image — this helps verify the producer (ESP32) is sending what the Pico expects.
  - Use a logic analyzer for stubborn issues: capture the TTL UART bytes and inspect timing, stray bytes, or missing frames.
  - If you need help converting an image format from the ESP32 to the driver's 4-bit-per-pixel packing, share the ESP32 sender code or a sample dump and I can propose a converter.

  ---

  If you want, I can add a short section describing how to add the `-DDISABLE_DISPLAY_SLEEP` flag into your current CMake flow (or I can open a small PR that makes it configurable in CMake).*** End Patch