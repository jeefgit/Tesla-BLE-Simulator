#pragma once

#include <stdint.h>

struct BoardProfile {
  const char* name;
  const char* displayController;
  const char* touchController;

  uint16_t width;
  uint16_t height;

  // Four data lines plus clock and chip select for the QSPI display bus.
  int8_t pinLcdSio0;
  int8_t pinLcdSio1;
  int8_t pinLcdSio2;
  int8_t pinLcdSio3;
  int8_t pinLcdPclk;
  int8_t pinLcdCs;
  int8_t pinLcdReset;

  int8_t pinTouchSda;
  int8_t pinTouchScl;
  int8_t pinTouchInt;
  int8_t pinTouchReset;
  uint8_t touchAddress;
};
