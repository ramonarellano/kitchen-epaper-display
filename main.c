#include <stdlib.h>
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

int main(void) {
  stdio_init_all();  // Initialize USB serial
  if (DEV_Module_Init() != 0) {
    return -1;
  }
  uart_init(UART_ID, UART_BAUD);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  uart_log("System started");
  uint8_t image_buffer[IMAGE_SIZE];
  int error = 0;
  int fallback_displayed = 0;
  while (1) {
    if (!error) {
      led_ok_pattern();
    } else {
      led_error_pattern();
    }
    if (!error) {
      if (request_and_receive_image(image_buffer, IMAGE_SIZE) == 0) {
        uart_log("Displaying image");
        // TODO: implement EPD_7in3f_display_BMP_from_buffer(image_buffer)
        // EPD_7in3f_display_BMP_from_buffer(image_buffer);
        uart_log("Image displayed");
      } else {
        if (!fallback_displayed) {
          uart_log("Image receive failed, displaying fallback image");
          display_fallback_image();
          uart_log("Fallback image displayed");
          fallback_displayed = 1;
        } else {
          uart_log("Image receive failed, fallback already displayed");
        }
        error = 1;
      }
    } else {
      uart_log("Retrying after error");
      DEV_Delay_ms(10000);
      error = 0;
    }
  }
}
