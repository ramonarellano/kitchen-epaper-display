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

### Troubleshooting: "Timeout or error waiting for SOF marker"

Summary
- The error "Timeout or error waiting for SOF marker" means the Pico never observed the agreed Start-Of-Frame (SOF) bytes from the ESP32. Common causes: mismatched UART settings, wrong pins, stray line endings (CR/LF), timing issues, or a different SOF/packet framing on the ESP32.

Quick checklist
- Verify both devices use the same UART settings (e.g. 115200, 8N1).
- Confirm Pico uses UART1 (GPIO4 = TX1, GPIO5 = RX1) and wiring to ESP32 TX/RX is correct (cross TX/RX).
- Disable any code on the ESP32 that adds human-readable prefixes (like "OK\n") before binary frames.
- Use a distinct binary SOF (2 bytes, not ASCII) e.g. 0x55 0xAA to avoid accidental matches.
- Test with a small payload (16 bytes) first.
- Enable hex logging on the Pico USB serial to see incoming bytes and timeouts.

Recommended framing (both sides must match)
- SOF: two bytes 0x55 0xAA
- LEN: 4-byte unsigned length (uint32_t, little-endian)
- PAYLOAD: LEN bytes of image data (raw binary)
- Optional: CRC or checksum after payload

ESP32 sender example (Arduino-style)
```cpp
// Example: send framed image from ESP32
// Uses SOF 0x55 0xAA then 4-byte little-endian length then payload
void sendFrameHardware(Stream &uart, const uint8_t *buf, uint32_t len) {
    uint8_t sof[2] = {0x55, 0xAA};
    uart.write(sof, 2);
    // send length little-endian
    uart.write((uint8_t*)&len, 4);
    // send payload in chunks to avoid saturating buffers
    const size_t CHUNK = 1024;
    uint32_t sent = 0;
    while (sent < len) {
        size_t toSend = min((uint32_t)CHUNK, len - sent);
        uart.write(buf + sent, toSend);
        uart.flush(); // ensure bytes are pushed out
        sent += toSend;
        delay(2); // small pause if needed
    }
}
```

RP2040 (Pico) receiver pseudocode / best practices
- Implement a sliding-window search for the 2-byte SOF with a short per-byte timeout.
- Once SOF is found, read exactly 4 bytes for length (handle endianness).
- Read LEN bytes with a read-loop that accumulates until complete or timeout.
- Log each step (SOF detection, length value, bytes received) as hex over USB serial.

Pseudocode:
```c
// Pseudocode: wait for SOF then read a length and payload with timeouts
uint8_t readByteWithTimeout(uart_inst_t *u, uint32_t timeout_ms, bool *ok);

bool wait_for_sof(uart_inst_t *u, uint32_t timeout_ms) {
    uint8_t win[2] = {0,0};
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        bool ok;
        uint8_t b = readByteWithTimeout(u, 50, &ok);
        if (!ok) continue;
        win[0] = win[1];
        win[1] = b;
        if (win[0]==0x55 && win[1]==0xAA) return true;
    }
    return false;
}

bool receive_frame(uart_inst_t *u) {
    if (!wait_for_sof(u, 5000)) {
        log("Timeout waiting for SOF");
        return false;
    }
    // read 4-byte length (little-endian)
    uint8_t lenbuf[4];
    if (!readFully(u, lenbuf, 4, 5000)) { log("Timeout reading length"); return false; }
    uint32_t len = lenbuf[0] | (lenbuf[1]<<8) | (lenbuf[2]<<16) | (lenbuf[3]<<24);
    logf("Frame length: %u", len);
    if (len == 0 || len > MAX_EXPECTED_IMAGE_SIZE) { log("Invalid length"); return false; }
    // read payload
    if (!readFully(u, imageBuffer, len, 10000)) { log("Timeout reading payload"); return false; }
    // Optionally verify CRC here
    return true;
}
```

Debug logging tips
- Print hex of every received byte and timestamps to USB serial until the issue is diagnosed.
- If you see ASCII text like "SENDIMG\n" or stray characters before SOF, fix ESP32 to send only the binary frame.
- Use a loopback test on each device to verify UART hardware (connect TX to RX and verify data).
- If frames are large, consider chunking or increasing read timeouts.

If these steps don't resolve the issue
- Capture the raw TTL signals with a logic analyzer and inspect the exact bytes/timing.
- Post the raw hex dump from the Pico (first 256 bytes) and the ESP32 sending code for more targeted help.

### Notes
- ePaper documentation: https://www.waveshare.com/wiki/7.3inch_e-Paper_HAT_(F)_Manual#Overview
- If you do not see your board as a serial device after flashing, ensure you have pressed the **RUN** button after copying the UF2 file.
- The image display function is a placeholder and should be implemented to match your image format and display requirements.
- The firmware will keep retrying the handshake with the ESP32 until it succeeds, even after displaying the fallback image.

---