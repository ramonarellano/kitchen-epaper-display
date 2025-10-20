#include <assert.h>
#include <stdlib.h>
#include <string.h>  // For strstr
#include "hardware/uart.h"
#include "lib/led/led.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------------------
// Configuration (all tunable constants live here)
// ---------------------------------------------------------------------------

// General

// UART configuration
#define UART_ID uart1
#define UART_BAUD 115200
#define UART_TX_PIN 4
#define UART_RX_PIN 5

// Image buffer size: 800x480 7-color (example); adjust if using different
// display.
#define IMAGE_SIZE 192000

// How often to request an image (in minutes)
#define IMAGE_REQUEST_INTERVAL_MINUTES 5

// Timeouts (milliseconds)
#define ACK_TIMEOUT_MS 10000    // wait up to 10s for ACK
#define SOF_TIMEOUT_MS 60000    // wait up to 60s for start-of-frame
#define DATA_TIMEOUT_MS 180000  // wait up to 180s for full image data
#define RETRY_WAIT_MS 3000      // wait 30s after a timeout before retry
#define POST_SEND_DELAY_MS 20   // small delay after sending request

// Buffer sizes
#define ACK_BUFFER_SIZE 64

// Logging helper (short wrapper)
#define LOG(msg) uart_log(msg)

#include "lib/Fonts/fonts.h"
#include "lib/GUI/GUI_Paint.h"
#include "lib/e-Paper/EPD_7in3f.h"

// ---------------------------------------------------------------------------
// End configuration
// ---------------------------------------------------------------------------

// (Removed unused helper functions: battery measurement and charge callback,
// and short LED patterns. LED status functions still used elsewhere.)

// LED status patterns
void led_status_ok(void) {
  // Slow blink: 200ms ON, 1800ms OFF
  DEV_Digital_Write(LED_ACT, 1);
  DEV_Delay_ms(200);
  DEV_Digital_Write(LED_ACT, 0);
  DEV_Delay_ms(1800);
}
void led_status_error(void) {
  // Fast blink: 200ms ON, 300ms OFF
  DEV_Digital_Write(LED_ACT, 1);
  DEV_Delay_ms(200);
  DEV_Digital_Write(LED_ACT, 0);
  DEV_Delay_ms(300);
}
void led_status_transferring(void) {
  // LED ON continuously
  DEV_Digital_Write(LED_ACT, 1);
}
void led_status_off(void) {
  DEV_Digital_Write(LED_ACT, 0);
}

/**
 * uart_log
 * Simple logging helper that writes to USB serial (stdio).
 */
void uart_log(const char* msg) {
  printf("LOG: %s\r\n", msg);
}

size_t last_receive_count = 0;

// Forward declarations for helper functions
static void flush_rx(void);
static void send_image_request(void);
static int contains_ack(const char* s);
static int read_ack_with_timeout(int32_t timeout_ms);
static int wait_for_sof(int32_t timeout_ms);
static int read_image_size(size_t* out_size);
static int receive_image_data(uint8_t* buffer,
                              size_t buf_size,
                              size_t img_size);

/**
 * request_and_receive_image
 * High-level flow:
 *  - send SENDIMG request
 *  - wait for ACK (robust, tolerant)
 *  - wait for SOF marker
 *  - read image size
 *  - read image data
 * Returns 0 on success, -1 on fatal error, -2 if timed out and caller should
 * retry (we already waited RETRY_WAIT_MS inside this function in that case).
 */
int request_and_receive_image(uint8_t* buffer, size_t size) {
  LOG("Requesting image from ESP32");

  // Clear any stale bytes before starting
  flush_rx();

  // Send request and give peer a short time to prepare
  send_image_request();
  sleep_ms(POST_SEND_DELAY_MS);

  // Wait for ACK
  LOG("Waiting for ACK from ESP32");
  if (read_ack_with_timeout(ACK_TIMEOUT_MS) != 0) {
    LOG("No ACK received within timeout - waiting before retry");
    last_receive_count = 0;
    sleep_ms(RETRY_WAIT_MS);
    return -2;
  }

  // Wait for frame start marker (SOF)
  LOG("ACK received, waiting for SOF marker");
  if (wait_for_sof(SOF_TIMEOUT_MS) != 0) {
    LOG("SOF not found within timeout - waiting before retry");
    last_receive_count = 0;
    sleep_ms(RETRY_WAIT_MS);
    return -2;
  }

  LOG("SOF marker received, reading image size header");
  size_t img_size = 0;
  if (read_image_size(&img_size) != 0) {
    LOG("Failed to read image size header");
    last_receive_count = 0;
    return -1;
  }
  char size_msg[64];
  snprintf(size_msg, sizeof(size_msg), "Image size header: %u bytes",
           (unsigned)img_size);
  LOG(size_msg);

  if (img_size > size) {
    LOG("Image size in header exceeds buffer size, aborting");
    last_receive_count = 0;
    return -1;
  }

  LOG("Receiving image data");
  int rc = receive_image_data(buffer, size, img_size);
  if (rc != 0) {
    // receive_image_data already logs and waits when appropriate
    last_receive_count = (rc > 0) ? (size_t)rc : 0;
    return -2;
  }

  LOG("Image received");
  last_receive_count = img_size;
  return 0;
}

// -------------------- Helper implementations --------------------

// Discard any available bytes on RX
static void flush_rx(void) {
  while (uart_is_readable(UART_ID)) {
    (void)uart_getc(UART_ID);
  }
}

// Send image request string
static void send_image_request(void) {
  uart_puts(UART_ID, "SENDIMG\n");
}

// Simple substring test for ACK (case-sensitive, matches anywhere)
static int contains_ack(const char* s) {
  if (!s)
    return 0;
  return strstr(s, "ACK") != NULL;
}

// Read until newline or timeout. Returns 0 if an ACK was seen, -1 otherwise.
static int read_ack_with_timeout(int32_t timeout_ms) {
  char ack_buf[ACK_BUFFER_SIZE];
  size_t idx = 0;
  absolute_time_t start = get_absolute_time();
  const int64_t timeout_us = (int64_t)timeout_ms * 1000;
  while (absolute_time_diff_us(start, get_absolute_time()) < timeout_us) {
    if (uart_is_readable(UART_ID)) {
      int c = uart_getc(UART_ID);
      if (c < 0)
        continue;
      if (idx < sizeof(ack_buf) - 1)
        ack_buf[idx++] = (char)c;
      ack_buf[idx] = '\0';
      if (c == '\n') {
        // strip trailing CR/LF
        while (idx > 0 &&
               (ack_buf[idx - 1] == '\n' || ack_buf[idx - 1] == '\r')) {
          ack_buf[--idx] = '\0';
        }
        if (contains_ack(ack_buf))
          return 0;
        idx = 0;
        ack_buf[0] = '\0';
      }
    }
  }
  return -1;
}

// Wait for SOF marker 0xAA 0x55 0xAA 0x55. Returns 0 on success, -1 on timeout.
static int wait_for_sof(int32_t timeout_ms) {
  uint8_t sof[4] = {0};
  size_t sof_idx = 0;
  absolute_time_t start = get_absolute_time();
  const int64_t timeout_us = (int64_t)timeout_ms * 1000;
  while (sof_idx < 4 &&
         absolute_time_diff_us(start, get_absolute_time()) < timeout_us) {
    if (uart_is_readable(UART_ID)) {
      uint8_t c = uart_getc(UART_ID);
      sof[sof_idx] = c;
      if ((sof_idx == 0 && c == 0xAA) || (sof_idx == 1 && c == 0x55) ||
          (sof_idx == 2 && c == 0xAA) || (sof_idx == 3 && c == 0x55)) {
        sof_idx++;
      } else {
        sof_idx = (c == 0xAA) ? 1 : 0;
      }
    }
  }
  return (sof_idx == 4) ? 0 : -1;
}

// Read 4-byte big-endian image size into out_size; returns 0 on success.
static int read_image_size(size_t* out_size) {
  if (!out_size)
    return -1;
  uint8_t header[4];
  size_t idx = 0;
  while (idx < 4) {
    if (uart_is_readable(UART_ID)) {
      header[idx++] = uart_getc(UART_ID);
    }
  }
  *out_size =
      (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
  return 0;
}

// Receive image data with a DATA_TIMEOUT_MS overall timeout.
// Returns 0 on success, -1 on timeout. On partial receive, returns -1 but
// last_receive_count is populated.
static int receive_image_data(uint8_t* buffer,
                              size_t buf_size,
                              size_t img_size) {
  if (!buffer)
    return -1;
  size_t received = 0;
  absolute_time_t start = get_absolute_time();
  absolute_time_t last_log = start;
  const int64_t timeout_us = (int64_t)DATA_TIMEOUT_MS * 1000;
  while (received < img_size) {
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(start, now) > timeout_us) {
      LOG("Timeout waiting for image data");
      last_receive_count = received;
      sleep_ms(RETRY_WAIT_MS);
      return -1;
    }
    if (absolute_time_diff_us(last_log, now) > 2000000) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Still waiting for image data... %u/%u bytes",
               (unsigned)received, (unsigned)img_size);
      LOG(msg);
      last_log = now;
    }
    if (uart_is_readable(UART_ID)) {
      buffer[received++] = uart_getc(UART_ID);
      if (received % 4096 == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Received %u/%u bytes", (unsigned)received,
                 (unsigned)img_size);
        LOG(msg);
      }
    }
  }
  return 0;
}

// Fallback image logic removed per request; timeouts now wait 30s and return -2
// to indicate a retry should be attempted by the caller.

// usb_log removed (unused). Use uart_log(...) where needed.

int main(void) {
  stdio_init_all();  // Initialize USB serial
  if (DEV_Module_Init() != 0) {
    return -1;
  }
  // Initialize UART1 for image transfer
  uart_init(UART_ID, UART_BAUD);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
  sleep_ms(1000);  // Wait for USB-CDC

  uart_log("System started");
  uint8_t image_buffer[IMAGE_SIZE];
  int last_status_ok = 0;  // 1=ok, 0=error
  unsigned long last_display_sum = 0;
  while (1) {
    // LED status based on last result
    if (last_status_ok) {
      led_status_ok();
    } else {
      led_status_error();
    }
    // Clear image buffer to avoid leftover pixels from previous transfers
    memset(image_buffer, 0xFF, IMAGE_SIZE);

    // Try to request and receive image
    int recv_result = request_and_receive_image(image_buffer, IMAGE_SIZE);
    if (recv_result == 0) {
      // Indicate transfer in progress
      led_status_transferring();
      uart_log("Displaying image");
      // Debug: print first 32 bytes of image_buffer
      char hexbuf[3 * 32 + 1] = {0};
      for (int i = 0; i < 32; ++i) {
        snprintf(hexbuf + i * 3, 4, "%02X ", image_buffer[i]);
      }
      uart_log("First 32 bytes of image_buffer:");
      uart_log(hexbuf);
      // Log final received count so it's clear we read the whole image (not
      // just the last periodic progress log which prints at 4096-byte
      // intervals).
      char finalrcv[64];
      snprintf(finalrcv, sizeof(finalrcv), "Final received: %u/%u bytes",
               (unsigned)last_receive_count, (unsigned)IMAGE_SIZE);
      uart_log(finalrcv);

      // Compute a simple checksum of the full received image to detect
      // identical images (avoid unnecessary redisplay which can be slow).
      unsigned long full_sum = 0;
      for (size_t i = 0; i < last_receive_count; ++i)
        full_sum += image_buffer[i];
      char summsg[80];
      snprintf(summsg, sizeof(summsg), "Full image checksum: %lu", full_sum);
      uart_log(summsg);

      if (last_display_sum != 0 && full_sum == last_display_sum) {
        uart_log(
            "Image identical to last displayed image — skipping redisplay");
        last_status_ok = 1;
        // wait IMAGE_REQUEST_INTERVAL_MINUTES before next request
        for (int i = IMAGE_REQUEST_INTERVAL_MINUTES; i > 0; --i) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Next update in %d minute%s...", i,
                   (i == 1) ? "" : "s");
          uart_log(msg);
          for (int j = 0; j < 60; ++j) {
            sleep_ms(1000);
          }
        }
        continue;
      }
      EPD_7IN3F_Init();
      uart_log("EPD_7IN3F_Init() done");
      // Log a simple checksum so we can verify the buffer changes between
      // updates. This helps detect whether the same image is being sent.
      unsigned long img_sum = 0;
      for (size_t _i = 0; _i < 32 && _i < IMAGE_SIZE; ++_i)
        img_sum += image_buffer[_i];
      char chkmsg[64];
      snprintf(chkmsg, sizeof(chkmsg), "Image checksum (first 32 bytes): %lu",
               img_sum);
      uart_log(chkmsg);
      EPD_7IN3F_Display(image_buffer);
      uart_log("EPD_7IN3F_Display() done");
      // Give the panel time to finish refresh before entering deep-sleep.
      // In some hardware/driver combos an immediate sleep can prevent the
      // visible update on subsequent refreshes.
      sleep_ms(5000);
#if !defined(DISABLE_DISPLAY_SLEEP)
  EPD_7IN3F_Sleep();
  uart_log("EPD_7IN3F_Sleep() done");
#else
  uart_log("EPD_7IN3F_Sleep() skipped (DISABLE_DISPLAY_SLEEP defined)");
#endif

      // Remember checksum of last displayed image
      last_display_sum = full_sum;
      uart_log("Image displayed");
      last_status_ok = 1;
      led_status_off();
      // Wait IMAGE_REQUEST_INTERVAL_MINUTES before next request, logging time
      // remaining every minute
      for (int i = IMAGE_REQUEST_INTERVAL_MINUTES; i > 0; --i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Next update in %d minute%s...", i,
                 (i == 1) ? "" : "s");
        uart_log(msg);
        for (int j = 0; j < 60; ++j) {
          sleep_ms(1000);
        }
      }
    } else if (recv_result == -2) {
      // request_and_receive_image already logged the timeout and waited 30s.
      uart_log("Retrying image request after timeout wait");
      last_status_ok = 0;
      // Continue to start the request sequence again immediately
      continue;
    } else {
      uart_log("Image reception failed, will retry.");
      last_status_ok = 0;
      // Retry every 5 seconds
      for (int i = 0; i < 5; ++i) {
        sleep_ms(1000);
      }
    }
  }
}
