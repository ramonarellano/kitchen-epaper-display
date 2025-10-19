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

  - Centralized configuration at the top of `main.c` (all timeouts and buffer sizes in one place).
  - Robust ACK detection and ACK timeout increased to 10s; SOF and data-timeout handling improved.
  - Removed fallback-image display (simplified flow) and made timeouts wait-and-retry in a consistent way.
  - Added a small host-test (`tests/test_ack.c`) that validates ACK substring detection.
  - Added a short delay before entering display sleep to ensure the panel finishes refresh; the sleep is now optional at build time with `DISABLE_DISPLAY_SLEEP`.

  If you upgraded from an earlier version and observed the display not updating after the first refresh, the added sleep delay or the option to skip sleep fixes a timing issue that could prevent visible updates.

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

  Build-time option:

  - `DISABLE_DISPLAY_SLEEP` — if defined at compile time, the firmware will skip putting the e-Paper display into deep sleep immediately after `EPD_7IN3F_Display()`. Useful during debugging or when rapid successive updates are required. Provide this via your build system (e.g. add `-DDISABLE_DISPLAY_SLEEP` to the compiler flags or add a `target_compile_definitions` entry in your CMake config).

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