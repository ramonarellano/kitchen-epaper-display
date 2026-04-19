/*****************************************************************************
* | File      	:   EPD_7in3f.c
* | Author      :   Waveshare team
* | Function    :   7.3inch e-Paper (F) Driver
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2022-08-04
* | Info        :
* -----------------------------------------------------------------------------
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "EPD_7in3f.h"
#include "pico/time.h"

// Phase timing exported so main.c can log them via PLOG.
// Set by TurnOnDisplay after each refresh cycle.
volatile int32_t epd_phase_power_on_ms = -1;
volatile int32_t epd_phase_refresh_ms = -1;
volatile int32_t epd_phase_power_off_ms = -1;
volatile int epd_busy_pin_at_init = -1;

// Global flag: incremented each time ReadBusyH force-releases due to timeout.
// Checked from main.c to detect incomplete e-paper operations.
volatile int epd_busy_force_released = 0;

/******************************************************************************
function :	Software reset
parameter:
******************************************************************************/
static void EPD_7IN3F_Reset(void) {
  DEV_Digital_Write(EPD_RST_PIN, 1);
  DEV_Delay_ms(20);
  DEV_Digital_Write(EPD_RST_PIN, 0);
  DEV_Delay_ms(50);  // 50ms reset pulse (was 5ms) — panel needs longer to
                     // wake from deep sleep
  DEV_Digital_Write(EPD_RST_PIN, 1);
  DEV_Delay_ms(300);  // 300ms post-reset (was 20ms) — give panel time to
                      // restart oscillator and assert BUSY before we check it
}

/******************************************************************************
function :	send command
parameter:
     Reg : Command register
******************************************************************************/
static void EPD_7IN3F_SendCommand(UBYTE Reg) {
  DEV_Digital_Write(EPD_DC_PIN, 0);
  DEV_Digital_Write(EPD_CS_PIN, 0);
  DEV_SPI_WriteByte(Reg);
  DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
******************************************************************************/
static void EPD_7IN3F_SendData(UBYTE Data) {
  DEV_Digital_Write(EPD_DC_PIN, 1);
  DEV_Digital_Write(EPD_CS_PIN, 0);
  DEV_SPI_WriteByte(Data);
  DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	Wait until the busy_pin goes LOW (with configurable timeout)
parameter:
  timeout_ms : maximum wait in milliseconds
returns   : 0 on success, -1 on timeout
******************************************************************************/
static int EPD_7IN3F_ReadBusyH_timeout(int timeout_ms) {
  int cnt = 0;
  int limit = timeout_ms / 10;
  printf("e-Paper busy H (timeout %dms)\r\n", timeout_ms);
  while (!DEV_Digital_Read(EPD_BUSY_PIN)) {  // LOW: busy, HIGH: idle
    DEV_Delay_ms(10);
    cnt++;
    if (cnt > limit) {
      printf("e-Paper busy H TIMEOUT after %d0 ms\r\n", cnt);
      epd_busy_force_released++;
      return -1;
    }
  }
  printf("e-Paper busy H release after %d0 ms\r\n", cnt);
  return 0;
}

// Default 120s timeout wrapper (used by Init, Clear, etc.)
static void EPD_7IN3F_ReadBusyH(void) {
  EPD_7IN3F_ReadBusyH_timeout(120000);
}
static void EPD_7IN3F_ReadBusyL(void) {
  printf("e-Paper busy L\r\n");
  while (DEV_Digital_Read(EPD_BUSY_PIN)) {  // LOW: idle, HIGH: busy
    DEV_Delay_ms(5);
  }
  printf("e-Paper busy L release\r\n");
}

/******************************************************************************
function :	Turn On Display (abort-safe: stops sending commands on timeout)
parameter:
returns   : 0 on success, -1 POWER_ON timeout, -2 REFRESH timeout,
            -3 POWER_OFF timeout
******************************************************************************/
static int EPD_7IN3F_TurnOnDisplay(void) {
  absolute_time_t t0;

  // POWER_ON — should complete in <1s.  10s timeout.
  t0 = get_absolute_time();
  EPD_7IN3F_SendCommand(0x04);  // POWER_ON
  int rc = EPD_7IN3F_ReadBusyH_timeout(10000);
  epd_phase_power_on_ms =
      (int32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
  if (rc != 0) {
    // Panel didn't power on — do NOT send REFRESH or POWER_OFF.
    // Sending commands to a stuck panel corrupts its state machine.
    epd_phase_refresh_ms = -1;
    epd_phase_power_off_ms = -1;
    return -1;
  }

  // DISPLAY_REFRESH — takes ~31s for 7-color.  60s timeout.
  t0 = get_absolute_time();
  EPD_7IN3F_SendCommand(0x12);  // DISPLAY_REFRESH
  EPD_7IN3F_SendData(0x00);
  rc = EPD_7IN3F_ReadBusyH_timeout(60000);
  epd_phase_refresh_ms =
      (int32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
  if (rc != 0) {
    // Refresh timed out — still send POWER_OFF to protect panel from HV damage
    t0 = get_absolute_time();
    EPD_7IN3F_SendCommand(0x02);  // POWER_OFF
    EPD_7IN3F_SendData(0x00);
    EPD_7IN3F_ReadBusyH_timeout(10000);  // best effort
    epd_phase_power_off_ms =
        (int32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
    return -2;
  }

  // POWER_OFF — should complete in ~150ms.  10s timeout.
  t0 = get_absolute_time();
  EPD_7IN3F_SendCommand(0x02);  // POWER_OFF
  EPD_7IN3F_SendData(0X00);
  rc = EPD_7IN3F_ReadBusyH_timeout(10000);
  epd_phase_power_off_ms =
      (int32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
  if (rc != 0) {
    return -3;  // POWER_OFF timeout — panel stays powered
  }

  return 0;
}

/******************************************************************************
function :	Initialize the e-Paper register
parameter:
returns   : 0 on success, -1 if panel didn't respond after reset
******************************************************************************/
int EPD_7IN3F_Init(void) {
  epd_busy_pin_at_init = DEV_Digital_Read(EPD_BUSY_PIN);
  EPD_7IN3F_Reset();
  // After deep sleep, the panel needs time to restart its internal oscillator
  // and assert BUSY LOW. Without this delay, ReadBusyH sees HIGH (idle)
  // immediately and all subsequent commands are ignored by the still-sleeping
  // panel — producing a 447ms "refresh" that doesn't physically update.
  DEV_Delay_ms(100);
  if (EPD_7IN3F_ReadBusyH_timeout(30000) != 0) {
    return -1;  // panel didn't come up after reset
  }
  DEV_Delay_ms(30);

  EPD_7IN3F_SendCommand(0xAA);  // CMDH
  EPD_7IN3F_SendData(0x49);
  EPD_7IN3F_SendData(0x55);
  EPD_7IN3F_SendData(0x20);
  EPD_7IN3F_SendData(0x08);
  EPD_7IN3F_SendData(0x09);
  EPD_7IN3F_SendData(0x18);

  EPD_7IN3F_SendCommand(0x01);
  EPD_7IN3F_SendData(0x3F);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x32);
  EPD_7IN3F_SendData(0x2A);
  EPD_7IN3F_SendData(0x0E);
  EPD_7IN3F_SendData(0x2A);

  EPD_7IN3F_SendCommand(0x00);
  EPD_7IN3F_SendData(0x5F);
  EPD_7IN3F_SendData(0x69);

  EPD_7IN3F_SendCommand(0x03);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x54);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x44);

  EPD_7IN3F_SendCommand(0x05);
  EPD_7IN3F_SendData(0x40);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x2C);

  EPD_7IN3F_SendCommand(0x06);
  EPD_7IN3F_SendData(0x6F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x22);

  EPD_7IN3F_SendCommand(0x08);
  EPD_7IN3F_SendData(0x6F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x22);

  EPD_7IN3F_SendCommand(0x13);  // IPC
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x04);

  EPD_7IN3F_SendCommand(0x30);
  EPD_7IN3F_SendData(0x3C);

  EPD_7IN3F_SendCommand(0x41);  // TSE
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x50);
  EPD_7IN3F_SendData(0x3F);

  EPD_7IN3F_SendCommand(0x60);
  EPD_7IN3F_SendData(0x02);
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x61);
  EPD_7IN3F_SendData(0x03);
  EPD_7IN3F_SendData(0x20);
  EPD_7IN3F_SendData(0x01);
  EPD_7IN3F_SendData(0xE0);

  EPD_7IN3F_SendCommand(0x82);
  EPD_7IN3F_SendData(0x1E);

  EPD_7IN3F_SendCommand(0x84);
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x86);  // AGID
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0xE3);
  EPD_7IN3F_SendData(0x2F);

  EPD_7IN3F_SendCommand(0xE0);  // CCSET
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0xE6);  // TSSET
  EPD_7IN3F_SendData(0x00);
  return 0;
}

/******************************************************************************
function :	Reload all register config WITHOUT hardware reset.
                        Use before each display cycle when the panel has been in
                        POWER_OFF standby for a long time (>minutes).  At
<0.01µA standby the controller SRAM loses register state. This is the same
register sequence as Init() minus Reset() + ReadBusyH(), so the BUSY pin is not
disturbed. parameter:
******************************************************************************/
void EPD_7IN3F_ReloadConfig(void) {
  EPD_7IN3F_SendCommand(0xAA);  // CMDH
  EPD_7IN3F_SendData(0x49);
  EPD_7IN3F_SendData(0x55);
  EPD_7IN3F_SendData(0x20);
  EPD_7IN3F_SendData(0x08);
  EPD_7IN3F_SendData(0x09);
  EPD_7IN3F_SendData(0x18);

  EPD_7IN3F_SendCommand(0x01);
  EPD_7IN3F_SendData(0x3F);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x32);
  EPD_7IN3F_SendData(0x2A);
  EPD_7IN3F_SendData(0x0E);
  EPD_7IN3F_SendData(0x2A);

  EPD_7IN3F_SendCommand(0x00);
  EPD_7IN3F_SendData(0x5F);
  EPD_7IN3F_SendData(0x69);

  EPD_7IN3F_SendCommand(0x03);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x54);
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x44);

  EPD_7IN3F_SendCommand(0x05);
  EPD_7IN3F_SendData(0x40);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x2C);

  EPD_7IN3F_SendCommand(0x06);
  EPD_7IN3F_SendData(0x6F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x22);

  EPD_7IN3F_SendCommand(0x08);
  EPD_7IN3F_SendData(0x6F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x1F);
  EPD_7IN3F_SendData(0x22);

  EPD_7IN3F_SendCommand(0x13);  // IPC
  EPD_7IN3F_SendData(0x00);
  EPD_7IN3F_SendData(0x04);

  EPD_7IN3F_SendCommand(0x30);
  EPD_7IN3F_SendData(0x3C);

  EPD_7IN3F_SendCommand(0x41);  // TSE
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x50);
  EPD_7IN3F_SendData(0x3F);

  EPD_7IN3F_SendCommand(0x60);
  EPD_7IN3F_SendData(0x02);
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x61);
  EPD_7IN3F_SendData(0x03);
  EPD_7IN3F_SendData(0x20);
  EPD_7IN3F_SendData(0x01);
  EPD_7IN3F_SendData(0xE0);

  EPD_7IN3F_SendCommand(0x82);
  EPD_7IN3F_SendData(0x1E);

  EPD_7IN3F_SendCommand(0x84);
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0x86);  // AGID
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0xE3);
  EPD_7IN3F_SendData(0x2F);

  EPD_7IN3F_SendCommand(0xE0);  // CCSET
  EPD_7IN3F_SendData(0x00);

  EPD_7IN3F_SendCommand(0xE6);  // TSSET
  EPD_7IN3F_SendData(0x00);
}

/******************************************************************************
function :	Power on the HV boost (0x04) and wait for BUSY.
                        Call after Init() and before Display() when the panel
has been in POWER_OFF for a long time.  GxEPD2 does this in _InitDisplay(); it
ensures the controller is fully awake before data writes. parameter:
******************************************************************************/
int EPD_7IN3F_PowerOn(void) {
  EPD_7IN3F_SendCommand(0x04);  // POWER_ON
  return EPD_7IN3F_ReadBusyH_timeout(10000);
}

/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void EPD_7IN3F_Clear(UBYTE color) {
  UWORD Width, Height;
  Width = (EPD_7IN3F_WIDTH % 2 == 0) ? (EPD_7IN3F_WIDTH / 2)
                                     : (EPD_7IN3F_WIDTH / 2 + 1);
  Height = EPD_7IN3F_HEIGHT;

  EPD_7IN3F_SendCommand(0x10);
  for (UWORD j = 0; j < Height; j++) {
    for (UWORD i = 0; i < Width; i++) {
      EPD_7IN3F_SendData((color << 4) | color);
    }
  }

  EPD_7IN3F_TurnOnDisplay();
}

/******************************************************************************
function :	show 7 kind of color block
parameter:
******************************************************************************/
void EPD_7IN3F_Show7Block(void) {
  unsigned long i, j, k;
  unsigned char const Color_seven[8] = {
      EPD_7IN3F_BLACK, EPD_7IN3F_BLUE,   EPD_7IN3F_GREEN, EPD_7IN3F_ORANGE,
      EPD_7IN3F_RED,   EPD_7IN3F_YELLOW, EPD_7IN3F_WHITE, EPD_7IN3F_WHITE};

  EPD_7IN3F_SendCommand(0x10);
  for (i = 0; i < 240; i++) {
    for (k = 0; k < 4; k++) {
      for (j = 0; j < 100; j++) {
        EPD_7IN3F_SendData((Color_seven[k] << 4) | Color_seven[k]);
      }
    }
  }
  for (i = 0; i < 240; i++) {
    for (k = 4; k < 8; k++) {
      for (j = 0; j < 100; j++) {
        EPD_7IN3F_SendData((Color_seven[k] << 4) | Color_seven[k]);
      }
    }
  }
  EPD_7IN3F_TurnOnDisplay();
}

/******************************************************************************
function :	Sends the image buffer in RAM to e-Paper and displays
parameter:
******************************************************************************/
int EPD_7IN3F_Display(UBYTE* Image) {
  UWORD Width, Height;
  Width = (EPD_7IN3F_WIDTH % 2 == 0) ? (EPD_7IN3F_WIDTH / 2)
                                     : (EPD_7IN3F_WIDTH / 2 + 1);
  Height = EPD_7IN3F_HEIGHT;

  EPD_7IN3F_SendCommand(0x10);
  for (UWORD j = 0; j < Height; j++) {
    for (UWORD i = 0; i < Width; i++) {
      EPD_7IN3F_SendData(Image[i + j * Width]);
    }
  }
  return EPD_7IN3F_TurnOnDisplay();
}

/******************************************************************************
function :	Enter sleep mode
parameter:
******************************************************************************/
void EPD_7IN3F_Sleep(void) {
  EPD_7IN3F_SendCommand(0x07);  // DEEP_SLEEP
  EPD_7IN3F_SendData(0XA5);
}
