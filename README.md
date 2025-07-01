# Kitchen e-Paper Display Firmware

[Original PhotoPainter Repository](https://github.com/waveshareteam/PhotoPainter)

## Overview
This project provides firmware for a Raspberry Pi Pico (or compatible RP2040 board) that receives images from an ESP32 over UART and displays them on a Waveshare 7.3" e-Paper display. The firmware also provides status feedback via onboard LEDs and logs messages to both UART and USB serial.

## Main Features

- **Image Transfer via UART:**
  - The Pico requests an image from the ESP32 by sending `SENDIMG` over UART.
  - It waits for the image data to be received into a buffer (with a 5-second timeout).
  - If successful, the image is displayed on the e-Paper display (custom display function to be implemented).
  - If image reception fails, a fallback message is shown on the display.

- **Fallback Display:**
  - If no image is received, the display shows "NO SERIAL CONNECTION" as a status message.

- **LED Status:**
  - LEDs indicate normal operation (single blink) or error state (triple blink).

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

- The firmware communicates with an ESP32 over UART0 (GPIO 0/1 at 115200 baud).
- USB serial is also available for logging and debugging.

### Notes

- If you do not see your board as a serial device after flashing, ensure you have pressed the **RUN** button after copying the UF2 file.
- The image display function is a placeholder and should be implemented to match your image format and display requirements.

---