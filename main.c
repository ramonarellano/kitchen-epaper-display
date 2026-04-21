#include <assert.h>
#include <stdarg.h>
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
// Diagnostic run: shortened to 5 minutes to test whether Bug #15 depends
// more on idle time than on refresh count.
#define IMAGE_REQUEST_INTERVAL_MINUTES 5

// Timeouts (milliseconds)
#define ACK_TIMEOUT_MS 10000    // wait up to 10s for ACK
#define SOF_TIMEOUT_MS 60000    // wait up to 60s for start-of-frame
#define DATA_TIMEOUT_MS 180000  // wait up to 180s for full image data
#define RETRY_WAIT_MS 30000     // wait 30s after a timeout before retry
#define POST_SEND_DELAY_MS 20   // small delay after sending request

// Buffer sizes
#define ACK_BUFFER_SIZE 64

// Remote logging to ESP32 via UART (set to 0 to disable)
#define PICO_UART_LOGGING 1

// Logging helper (short wrapper)
#define LOG(msg) uart_log(msg)

#include "lib/Fonts/fonts.h"
#include "lib/GUI/GUI_Paint.h"
#include "lib/e-Paper/EPD_7in3f.h"

// Defined in EPD_7in3f.c — incremented on ReadBusyH force-release timeout.
extern volatile int epd_busy_force_released;
// Defined in EPD_7in3f.c — BUSY pin state 2ms after Reset().
extern volatile int epd_busy_after_reset;
// Defined in EPD_7in3f.c — BUSY pin state immediately before/after
// POWER_ON (0x04) and DISPLAY_REFRESH (0x12) inside TurnOnDisplay().
extern volatile int epd_busy_before_cmd04;
extern volatile int epd_busy_after_cmd04;
extern volatile int epd_busy_before_cmd12;
extern volatile int epd_busy_after_cmd12;

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

// ---------------------------------------------------------------------------
// Remote logging: buffer messages and send to ESP32 before next SENDIMG.
// The ESP32 is in deep sleep during Pico processing, so we must defer
// sending until right before the next image request, when the ESP32 is
// awake and listening on Serial1.
// ---------------------------------------------------------------------------
#if PICO_UART_LOGGING
#define PLOG_BUFFER_SIZE 4096
static char plog_buffer[PLOG_BUFFER_SIZE];
static size_t plog_buffer_len = 0;

static void plog(const char* msg) {
  size_t needed = 5 + strlen(msg) + 1;  // "PLOG:" + msg + "\n"
  if (plog_buffer_len + needed < PLOG_BUFFER_SIZE) {
    plog_buffer_len +=
        snprintf(plog_buffer + plog_buffer_len,
                 PLOG_BUFFER_SIZE - plog_buffer_len, "PLOG:%s\n", msg);
  }
}

static void plog_fmt(const char* fmt, ...) {
  char tmp[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  plog(tmp);
}

// Flush buffered PLOG lines to UART (call before SENDIMG)
static void plog_flush(void) {
  if (plog_buffer_len > 0) {
    uart_puts(UART_ID, plog_buffer);
    sleep_ms(100);  // let ESP32 process lines before SENDIMG
    plog_buffer_len = 0;
    plog_buffer[0] = '\0';
  }
}
#else
#define plog(msg) ((void)0)
#define plog_fmt(...) ((void)0)
#define plog_flush() ((void)0)
#endif

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

// Read 4-byte big-endian image size into out_size; returns 0 on success,
// -1 on timeout (10 s).
static int read_image_size(size_t* out_size) {
  if (!out_size)
    return -1;
  uint8_t header[4];
  size_t idx = 0;
  absolute_time_t start = get_absolute_time();
  const int64_t timeout_us = 10000000;  // 10 seconds
  while (idx < 4) {
    if (absolute_time_diff_us(start, get_absolute_time()) > timeout_us) {
      LOG("Timeout reading image size header");
      return -1;
    }
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
  static uint8_t image_buffer[IMAGE_SIZE];
  int last_status_ok = 0;  // 1=ok, 0=error
  unsigned long last_display_sum = 0;
  unsigned int cycle_count = 0;
  unsigned int total_sendimg_attempts = 0;
  int vbus = gpio_get(24);  // VBUS: 1=USB host, 0=wall/battery
  plog_fmt("BOOT vbus=%d fw=FULL_REINIT_v4_5MIN_SPLIT", vbus);

  // Initialize e-paper at boot so the first cycle starts from a known state.
  // Later cycles still do a per-cycle re-init while Bug #15 is under test.
  plog("EPD_INIT_BOOT");
  int boot_rc = EPD_7IN3F_Init();
  plog_fmt("EPD_INIT_BOOT_DONE busy_before=%d busy_after_rst=%d rc=%d",
           epd_busy_pin_at_init, epd_busy_after_reset, boot_rc);

  while (1) {
    // LED status based on last result
    if (last_status_ok) {
      led_status_ok();
    } else {
      led_status_error();
    }
    // Clear image buffer to avoid leftover pixels from previous transfers
    memset(image_buffer, 0xFF, IMAGE_SIZE);

    // Log cycle diagnostics
    cycle_count++;
    total_sendimg_attempts++;
    vbus = gpio_get(24);
    plog_fmt("CYCLE %u vbus=%d last_sum=%lu attempts=%u", cycle_count, vbus,
             last_display_sum, total_sendimg_attempts);

    // Flush any buffered PLOG lines to ESP32 before sending SENDIMG.
    // The ESP32 is awake and listening at this point.
    plog_flush();

    // Try to request and receive image
    plog("SENDIMG_START");
    int recv_result = request_and_receive_image(image_buffer, IMAGE_SIZE);
    plog_fmt("SENDIMG_RESULT rc=%d recv=%u", recv_result,
             (unsigned)last_receive_count);
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
        plog_fmt("SKIP chk=%lu last=%lu bytes=%u", full_sum, last_display_sum,
                 (unsigned)last_receive_count);
        uart_log(
            "Image identical to last displayed image — skipping redisplay");
        last_status_ok = 1;
        // wait IMAGE_REQUEST_INTERVAL_MINUTES before next request
        plog_fmt("SKIP_WAIT_START min=%d", IMAGE_REQUEST_INTERVAL_MINUTES);
        for (int i = IMAGE_REQUEST_INTERVAL_MINUTES; i > 0; --i) {
          if (i % 10 == 0 && i != IMAGE_REQUEST_INTERVAL_MINUTES) {
            plog_fmt("WAIT_TICK min_left=%d", i);
          }
          char msg[64];
          snprintf(msg, sizeof(msg), "Next update in %d minute%s...", i,
                   (i == 1) ? "" : "s");
          uart_log(msg);
          for (int j = 0; j < 60; ++j) {
            sleep_ms(1000);
          }
        }
        plog("WAIT_DONE");
        continue;
      }
      plog_fmt("DISPLAY chk=%lu bytes=%u first4=%02X%02X%02X%02X", full_sum,
               (unsigned)last_receive_count, image_buffer[0], image_buffer[1],
               image_buffer[2], image_buffer[3]);
      // Full hardware re-init + PowerOn before each display cycle.
      // Retry up to 3 times if Init or PowerOn times out.
      // Previous version (v1) forced through stuck BUSY, which corrupted
      // the panel's state machine.  v2 aborts on timeout and retries
      // with a fresh hardware reset.
#define MAX_REINIT_RETRIES 3
      int reinit_ok = 0;
      for (int attempt = 1; attempt <= MAX_REINIT_RETRIES; attempt++) {
        if (attempt > 1) {
          plog_fmt("REINIT_RETRY attempt=%d", attempt);
          DEV_Delay_ms(1000);  // extra settle time between retries
        }
        plog("FULL_REINIT");
        int init_rc = EPD_7IN3F_Init();
        plog_fmt(
            "REINIT_DONE busy_before=%d busy_after_rst=%d rc=%d attempt=%d",
            epd_busy_pin_at_init, epd_busy_after_reset, init_rc, attempt);
        if (init_rc != 0) {
          plog_fmt("INIT_TIMEOUT attempt=%d", attempt);
          continue;
        }
        int pon_rc = EPD_7IN3F_PowerOn();
        plog_fmt("POWER_ON_PRE rc=%d busy=%d->%d attempt=%d", pon_rc,
                 epd_busy_before_cmd04, epd_busy_after_cmd04, attempt);
        if (pon_rc != 0) {
          plog_fmt("POWER_ON_TIMEOUT attempt=%d", attempt);
          continue;
        }
        reinit_ok = 1;
        break;
      }
      if (!reinit_ok) {
        plog("REINIT_FAILED — skipping display this cycle");
        uart_log("Init+PowerOn failed after retries — skipping display");
        last_status_ok = 0;
      } else {
        epd_busy_force_released = 0;  // reset before Display
        // Log a simple checksum so we can verify the buffer changes between
        // updates. This helps detect whether the same image is being sent.
        unsigned long img_sum = 0;
        for (size_t _i = 0; _i < 32 && _i < IMAGE_SIZE; ++_i)
          img_sum += image_buffer[_i];
        char chkmsg[64];
        snprintf(chkmsg, sizeof(chkmsg), "Image checksum (first 32 bytes): %lu",
                 img_sum);
        uart_log(chkmsg);
        int forced_before_display = epd_busy_force_released;
        absolute_time_t disp_t0 = get_absolute_time();
        int disp_rc = EPD_7IN3F_Display(image_buffer);
        int64_t disp_us = absolute_time_diff_us(disp_t0, get_absolute_time());
        int forced_during_display =
            epd_busy_force_released - forced_before_display;
        uart_log("EPD_7IN3F_Display() done");
        plog_fmt("DISPLAY_DONE ms=%lld forced=%d rc=%d", disp_us / 1000,
                 forced_during_display, disp_rc);
        plog_fmt("EPD_PHASES pwr_on=%ld refresh=%ld pwr_off=%ld",
                 (long)epd_phase_power_on_ms, (long)epd_phase_refresh_ms,
                 (long)epd_phase_power_off_ms);
        plog_fmt("EPD_BUSY04 %d->%d", epd_busy_before_cmd04,
                epd_busy_after_cmd04);
        plog_fmt("EPD_BUSY12 %d->%d", epd_busy_before_cmd12,
                epd_busy_after_cmd12);
        // Real refresh: phase > 5s AND no timeout (rc==0 and no forced)
        int real_refresh = (disp_rc == 0 && epd_phase_refresh_ms > 5000 &&
                            forced_during_display == 0)
                               ? 1
                               : 0;
        plog_fmt("REFRESH_VERDICT real=%d refresh_ms=%ld disp_rc=%d",
                 real_refresh, (long)epd_phase_refresh_ms, disp_rc);
        uart_log("Image displayed");
        last_status_ok = 1;
      }
      // Standby + wait (always, whether display succeeded or was skipped)
      plog("STANDBY last_sum=0");
      last_display_sum = 0;
      led_status_off();
      // Wait IMAGE_REQUEST_INTERVAL_MINUTES before next request.
      // Log heartbeats every 10 minutes when the interval is long enough
      // so we can verify the Pico survived the wait.
      plog_fmt("WAIT_START min=%d", IMAGE_REQUEST_INTERVAL_MINUTES);
      for (int i = IMAGE_REQUEST_INTERVAL_MINUTES; i > 0; --i) {
        if (i % 10 == 0 && i != IMAGE_REQUEST_INTERVAL_MINUTES) {
          plog_fmt("WAIT_TICK min_left=%d", i);
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Next update in %d minute%s...", i,
                 (i == 1) ? "" : "s");
        uart_log(msg);
        for (int j = 0; j < 60; ++j) {
          sleep_ms(1000);
        }
      }
      plog("WAIT_DONE");
    } else if (recv_result == -2) {
      // request_and_receive_image already logged the timeout and waited 30s.
      uart_log("Retrying image request after timeout wait");
      plog_fmt("RECV_TIMEOUT retry attempts=%u", total_sendimg_attempts);
      last_status_ok = 0;
      // Continue to start the request sequence again immediately
      continue;
    } else {
      uart_log("Image reception failed, will retry.");
      plog_fmt("RECV_FAIL rc=%d attempts=%u", recv_result,
               total_sendimg_attempts);
      last_status_ok = 0;
      // Retry every 5 seconds
      for (int i = 0; i < 5; ++i) {
        sleep_ms(1000);
      }
    }
  }
}
