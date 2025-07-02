#include <stdlib.h>
#include <string.h>  // For strstr
#include "hardware/uart.h"
#include "lib/led/led.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

#define Mode 3
#define UART_ID uart0
#define UART_BAUD 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define IMAGE_SIZE (800 * 480 / 8)  // Example for 1bpp 800x480

#include "lib/Fonts/fonts.h"
#include "lib/GUI/GUI_Paint.h"
#include "lib/e-Paper/EPD_7in3f.h"

#define UART1_ID uart1
#define UART1_BAUD 115200
#define UART1_TX_PIN 4
#define UART1_RX_PIN 5
#define HANDSHAKE_MSG "HELLO ESP32\n"
#define HANDSHAKE_REPLY "HELLO RP2040\n"
#define HANDSHAKE_TIMEOUT_MS 2000
#define HANDSHAKE_RETRIES 5

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
  uart_puts(UART_ID, "LOG: ");
  uart_puts(UART_ID, msg);
  uart_puts(UART_ID, "\r\n");
  printf("LOG: %s\r\n", msg);  // Also log to USB serial
}

int request_and_receive_image(uint8_t* buffer, size_t size) {
  uart_log("Requesting image from ESP32");
  uart_puts(UART_ID, "SENDIMG\n");
  size_t received = 0;
  absolute_time_t start = get_absolute_time();
  while (received < size) {
    if (absolute_time_diff_us(start, get_absolute_time()) > 5000000) {
      uart_log("Timeout waiting for image data");
      return -1;
    }
    if (uart_is_readable(UART_ID)) {
      buffer[received++] = uart_getc(UART_ID);
    }
  }
  uart_log("Image received");
  return 0;
}

const uint8_t fallback_image[IMAGE_SIZE] = {[0 ... IMAGE_SIZE - 1] = 0xFF};

void display_fallback_image(void) {
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
  EPD_7IN3F_Display(BlackImage);
  EPD_7IN3F_Sleep();
  free(BlackImage);
}

// Helper: log to USB serial only
void usb_log(const char* msg) {
  printf("%s\n", msg);
}

// Wait for handshake reply on UART1
bool wait_for_handshake_reply(const char* expected, uint timeout_ms) {
  char buf[64] = {0};
  uint idx = 0;
  absolute_time_t start = get_absolute_time();
  while (to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(start) <
         timeout_ms) {
    if (uart_is_readable(UART1_ID)) {
      char c = uart_getc(UART1_ID);
      if (idx < sizeof(buf) - 1)
        buf[idx++] = c;
      if (strstr(buf, expected)) {
        usb_log("Received reply: HELLO RP2040");
        return true;
      }
    }
  }
  return false;
}

int main(void) {
  stdio_init_all();  // Initialize USB serial
  if (DEV_Module_Init() != 0) {
    return -1;
  }
  // Initialize UART0 for image transfer (legacy logic)
  uart_init(UART_ID, UART_BAUD);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
  // Initialize UART1 for handshake with ESP32
  uart_init(UART1_ID, UART1_BAUD);
  gpio_set_function(UART1_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART1_RX_PIN, GPIO_FUNC_UART);
  sleep_ms(1000);  // Wait for USB-CDC

  // Handshake logic
  for (int attempt = 1; attempt <= HANDSHAKE_RETRIES; ++attempt) {
    usb_log("Sending handshake...");
    uart_puts(UART1_ID, HANDSHAKE_MSG);
    if (wait_for_handshake_reply(HANDSHAKE_REPLY, HANDSHAKE_TIMEOUT_MS)) {
      usb_log("Handshake complete");
      break;
    } else {
      usb_log("Timeout waiting for reply, retrying...");
      sleep_ms(500);
    }
    if (attempt == HANDSHAKE_RETRIES) {
      usb_log("Handshake failed after retries.");
    }
  }

  uart_log("System started");
  uint8_t image_buffer[IMAGE_SIZE];
  int error = 0;
  int fallback_displayed = 0;
  int last_status_ok = 0;  // 1=ok, 0=error
  while (1) {
    // LED status based on last result
    if (last_status_ok) {
      led_status_ok();
    } else {
      led_status_error();
    }
    if (!error) {
      if (request_and_receive_image(image_buffer, IMAGE_SIZE) == 0) {
        // Indicate transfer in progress
        led_status_transferring();
        uart_log("Displaying image");
        // TODO: implement EPD_7in3f_display_BMP_from_buffer(image_buffer)
        // EPD_7in3f_display_BMP_from_buffer(image_buffer);
        uart_log("Image displayed");
        last_status_ok = 1;
        led_status_off();
      } else {
        if (!fallback_displayed) {
          uart_log("Image receive failed, displaying fallback image");
          display_fallback_image();
          uart_log("Fallback image displayed");
          fallback_displayed = 1;
        } else {
          uart_log("Image receive failed, fallback already displayed");
        }
        last_status_ok = 0;
      }
    } else {
      uart_log("Retrying after error");
      DEV_Delay_ms(10000);
      error = 0;
    }
  }
}
