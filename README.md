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
  - If successful, the image is displayed on the e-Paper display.
  - If image reception fails, a fallback message is shown on the display.

- **Fallback Display:**
  - If handshake or image transfer fails, the display shows "NO SERIAL CONNECTION" as a status message.

- **LED Status Patterns:**
  - **Slow blink (200ms ON, 1800ms OFF):** Last handshake or transfer was successful.
  - **Fast blink (200ms ON, 300ms OFF):** Last handshake or transfer failed.
  - **Solid ON:** Image transfer in progress.

- **Logging:**
  - Logs are sent to both UART and USB serial for debugging and monitoring.

- **Battery Measurement:**
  - The firmware measures battery voltage and logs it for diagnostics.

## Usage

### Building and Flashing the Firmware

1. Make sure you have the Pico SDK and all dependencies cloned (already handled by the setup scripts).

2. To mount your Raspberry Pi Pico as a USB drive (`RPI-RP2`), use this button sequence:
   - Press and hold the **RUN** button.
   - While holding **RUN**, press and hold the **BOOTSEL** button.
   - Release the **RUN** button (keep holding **BOOTSEL**).
   - Release the **BOOTSEL** button.
   - The board should now appear as a USB drive named `RPI-RP2`.

3. Run the build and flash script:

   ```sh
   ./build_and_flash.sh
   ```

   This script will:
   - Build the firmware using CMake and Make.
   - Copy the generated UF2 file to the `RPI-RP2` drive.

4. After the UF2 file is copied, press the **RUN** (or RESET) button on your Pico to start the firmware.

### Serial Communication

- **UART1 (GPIO 4/5):** Handshake and image transfer with ESP32 at 115200 baud.
- **USB serial:** Available for logging and debugging.

### LED Status Reference

- **Slow blink:** Last handshake or image transfer was successful.
- **Fast blink:** Last handshake or image transfer failed.
- **Solid ON:** Image transfer in progress.

### Notes

- If you do not see your board as a serial device after flashing, ensure you have pressed the **RUN** button after copying the UF2 file.
- The image display function is a placeholder and should be implemented to match your image format and display requirements.
- The firmware will keep retrying the handshake with the ESP32 until it succeeds, even after displaying the fallback image.

---