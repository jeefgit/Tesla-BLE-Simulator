#pragma once

#include "config.h"
#include "platform/BoardProfile.h"

// ---------------------------------------------------------------------------
// ESP32-2432S028R（CYD 2.8" 320×240 ILI9341）——当前默认板
//
// 参数来自 config.h 的实测值；新增板子时请复制本文件并改成新板实测值，
// 不要直接改这里的默认板参数。
// ---------------------------------------------------------------------------
inline constexpr BoardProfile BOARD_CYD_2432S028 = {
    /*name=*/"ESP32-2432S028R (CYD 2.8\")",
    /*width=*/CFG_LCD_WIDTH,
    /*height=*/CFG_LCD_HEIGHT,
    /*panelWidth=*/320,
    /*panelHeight=*/240,
    /*rotation=*/CFG_LCD_ROTATION,
    /*offsetRotation=*/CFG_LCD_OFFSET_ROTATION,
    /*flushRotation=*/CFG_LCD_FLUSH_ROTATION,
    /*pinSck=*/CFG_LCD_SCK,
    /*pinMosi=*/CFG_LCD_MOSI,
    /*pinMiso=*/CFG_LCD_MISO,
    /*pinCs=*/CFG_LCD_CS,
    /*pinDc=*/CFG_LCD_DC,
    /*pinRst=*/CFG_LCD_RST,
    /*pinBl=*/CFG_LCD_BL,
    /*pinTfCs=*/5,
    /*pinTouchCs=*/33,
    /*spiHz=*/CFG_LCD_SPI_FREQ,
    /*invert=*/CFG_LCD_INVERT != 0,
    /*rgbOrder=*/CFG_LCD_RGB_ORDER != 0,
    /*driverIli9341=*/CFG_LCD_DRIVER_ILI9341 != 0,
    /*bufferLines=*/24,
};
