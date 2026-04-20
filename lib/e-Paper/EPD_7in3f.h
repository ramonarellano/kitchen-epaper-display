/*****************************************************************************
* | File      	:   EPD_7in3f.h
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
#ifndef __EPD_7IN3F_H_
#define __EPD_7IN3F_H_

#include "DEV_Config.h"

// Display resolution
#define EPD_7IN3F_WIDTH 800
#define EPD_7IN3F_HEIGHT 480

/**********************************
Color Index
**********************************/
#define EPD_7IN3F_BLACK 0x0   /// 000
#define EPD_7IN3F_WHITE 0x1   ///	001
#define EPD_7IN3F_GREEN 0x2   ///	010
#define EPD_7IN3F_BLUE 0x3    ///	011
#define EPD_7IN3F_RED 0x4     ///	100
#define EPD_7IN3F_YELLOW 0x5  ///	101
#define EPD_7IN3F_ORANGE 0x6  ///	110
#define EPD_7IN3F_CLEAN 0x7   ///	111   unavailable  Afterimage

int EPD_7IN3F_Init(void);
void EPD_7IN3F_ReloadConfig(void);
int EPD_7IN3F_PowerOn(void);
void EPD_7IN3F_Clear(UBYTE color);
void EPD_7IN3F_Show7Block(void);
int EPD_7IN3F_Display(UBYTE* Image);
void EPD_7IN3F_Sleep(void);

// Phase timing (ms) from last TurnOnDisplay; -1 if not yet run.
extern volatile int32_t epd_phase_power_on_ms;
extern volatile int32_t epd_phase_refresh_ms;
extern volatile int32_t epd_phase_power_off_ms;
// BUSY pin state sampled at start of Init (before Reset).
extern volatile int epd_busy_pin_at_init;
// BUSY pin state sampled 2ms after Reset completes.
// 0 = LOW (panel is resetting — good). 1 = HIGH (reset didn't take).
extern volatile int epd_busy_after_reset;
// BUSY pin state sampled immediately before/after the last POWER_ON (0x04)
// command. Logged once after EPD_7IN3F_PowerOn() and again after Display().
extern volatile int epd_busy_before_cmd04;
extern volatile int epd_busy_after_cmd04;
// BUSY pin state sampled immediately before/after DISPLAY_REFRESH (0x12).
extern volatile int epd_busy_before_cmd12;
extern volatile int epd_busy_after_cmd12;

#endif
