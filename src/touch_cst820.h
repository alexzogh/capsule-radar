#pragma once
// Minimal Arduino driver for the CST820 capacitive touch (I2C). BOARD_LCD21 only.
// Same public API as the CST9217 driver so the display layer is board-agnostic.
// Protocol ported from Waveshare's Touch_CST820 demo.
#include <stdint.h>

bool touch_begin();                        // reset (via TCA9554) + comms probe; logs status
bool touch_read(uint16_t *x, uint16_t *y); // true if currently pressed (x,y in screen px)
