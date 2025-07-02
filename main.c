#include <stdlib.h>
#include <string.h>  // For strstr
#include "hardware/uart.h"
#include "lib/led/led.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

#define Mode 3
#define UART_ID uart1
#define UART_BAUD 115200
#define UART_TX_PIN 4
#define UART_RX_PIN 5
#define IMAGE_SIZE 192000  // 192,000 bytes for 800x480 (Waveshare 7-color)

#include "lib/Fonts/fonts.h"
#include "lib/GUI/GUI_Paint.h"
#include "lib/e-Paper/EPD_7in3f.h"

float measureVBAT(void) {
  float Voltage = 0.0;
  const float conversion_factor = 3.3f / (1 << 12);
  uint16_t result = adc_read();
  Voltage = result * conversion_factor * 3;
  printf("Raw value: 0x%03x, voltage: %f V\n", result, Voltage);
  return Voltage;
}

void chargeState_callback() {
  if (DEV_Digital_Read(VBUS)) {
    if (!DEV_Digital_Read(CHARGE_STATE)) {  // is charging
      ledCharging();
    } else {  // charge complete
      ledCharged();
    }
  }
}

void led_ok_pattern(void) {
  DEV_Digital_Write(LED_ACT, 1);
  DEV_Delay_ms(100);
  DEV_Digital_Write(LED_ACT, 0);
  DEV_Delay_ms(400);
}
void led_error_pattern(void) {
  for (int i = 0; i < 3; i++) {
    DEV_Digital_Write(LED_ACT, 1);
    DEV_Delay_ms(100);
    DEV_Digital_Write(LED_ACT, 0);
    DEV_Delay_ms(100);
  }
  DEV_Delay_ms(700);
}

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

void uart_log(const char* msg) {
  printf("LOG: %s\r\n", msg);  // Only log to USB serial
}

size_t last_receive_count = 0;

int request_and_receive_image(uint8_t* buffer, size_t size) {
  uart_log("Requesting image from ESP32");
  uart_puts(UART_ID, "SENDIMG\n");
  size_t received = 0;
  absolute_time_t start = get_absolute_time();
  absolute_time_t last_log = start;
  // Increase timeout to 25 seconds for large image
  while (received < size) {
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(start, now) > 250000000) {  // 250s timeout
      uart_log("Timeout waiting for image data");
      last_receive_count = received;
      return -1;
    }
    // Log every 2 seconds while waiting
    if (absolute_time_diff_us(last_log, now) > 2000000) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Still waiting for image data... %u/%u bytes",
               (unsigned)received, (unsigned)size);
      uart_log(msg);
      last_log = now;
    }
    if (uart_is_readable(UART_ID)) {
      buffer[received++] = uart_getc(UART_ID);
      if (received % 4096 == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Received %u/%u bytes", (unsigned)received,
                 (unsigned)size);
        uart_log(msg);
      }
    }
  }
  uart_log("Image received");
  last_receive_count = received;
  return 0;
}

const uint8_t fallback_image[IMAGE_SIZE] = {[0 ... IMAGE_SIZE - 1] = 0xFF};

void display_fallback_image(void) {
  uart_log("Resetting e-Paper display before fallback image");
  // Try to manually toggle the reset pin if available
#ifdef EPD_7IN3F_RST_PIN
  gpio_init(EPD_7IN3F_RST_PIN);
  gpio_set_dir(EPD_7IN3F_RST_PIN, GPIO_OUT);
  gpio_put(EPD_7IN3F_RST_PIN, 0);
  sleep_ms(200);
  gpio_put(EPD_7IN3F_RST_PIN, 1);
  sleep_ms(200);
#endif
  uart_log("Initializing e-Paper display for fallback image");
  EPD_7IN3F_Init();
  UDOUBLE Imagesize = ((EPD_7IN3F_WIDTH % 2 == 0) ? (EPD_7IN3F_WIDTH / 2)
                                                  : (EPD_7IN3F_WIDTH / 2 + 1)) *
                      EPD_7IN3F_HEIGHT;
  UBYTE* BlackImage = (UBYTE*)malloc(Imagesize);
  if (!BlackImage) {
    uart_log("Failed to allocate memory for fallback image");
    return;
  }
  Paint_NewImage(BlackImage, EPD_7IN3F_WIDTH, EPD_7IN3F_HEIGHT, 0,
                 EPD_7IN3F_WHITE);
  Paint_SetScale(7);
  Paint_SelectImage(BlackImage);
  Paint_Clear(EPD_7IN3F_WHITE);
  Paint_DrawString_EN(100, 220, "NO SERIAL CONNECTION", &Font24,
                      EPD_7IN3F_BLACK, EPD_7IN3F_WHITE);
  uart_log("Displaying fallback image on e-Paper");
  EPD_7IN3F_Display(BlackImage);
  EPD_7IN3F_Sleep();
  free(BlackImage);
}

// Helper: log to USB serial only
void usb_log(const char* msg) {
  printf("%s\n", msg);
}

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
  int fallback_displayed = 0;
  int last_status_ok = 0;  // 1=ok, 0=error
  while (1) {
    // LED status based on last result
    if (last_status_ok) {
      led_status_ok();
    } else {
      led_status_error();
    }
    // Try to request and receive image
    if (request_and_receive_image(image_buffer, IMAGE_SIZE) == 0) {
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
      EPD_7IN3F_Init();
      uart_log("EPD_7IN3F_Init() done");
      EPD_7IN3F_Display(image_buffer);
      uart_log("EPD_7IN3F_Display() done");
      EPD_7IN3F_Sleep();
      uart_log("EPD_7IN3F_Sleep() done");
      uart_log("Image displayed");
      last_status_ok = 1;
      led_status_off();
      fallback_displayed = 0;
      // Wait 5 minutes before next request
      for (int i = 0; i < 300; ++i) {
        sleep_ms(1000);
      }
    } else {
      uart_log("Image reception failed, will retry.");
      if (!fallback_displayed) {
        uart_log("Image receive failed, displaying fallback image (entering)");
        // Only display fallback if we received at least 1 byte last time
        extern size_t last_receive_count;  // declare external if needed
        if (last_receive_count > 0) {
          display_fallback_image();
          uart_log("Fallback image displayed (exiting)");
        } else {
          uart_log(
              "No data received, skipping fallback image to avoid e-Paper busy "
              "lockup");
        }
        sleep_ms(1000);  // Give hardware time to settle
        fallback_displayed = 1;
      } else {
        uart_log("Image receive failed, fallback already displayed");
      }
      last_status_ok = 0;
      // Retry every 5 seconds
      for (int i = 0; i < 5; ++i) {
        sleep_ms(1000);
      }
    }
  }
}
