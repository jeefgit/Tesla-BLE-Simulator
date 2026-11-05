#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// BoardProfile：一块「ESP32 板 + 屏」的硬件描述
//
// 换板/换屏时只新增一个 platform/boards/xxx.h（填这张表），
// 然后通过编译宏 -DBOARD_XXX 选择；业务代码与 UI 均不感知引脚。
//
// 注意：panel/rotation/offsetRotation 是「实测稳定组合」，换屏前先看
// docs 里的屏幕调试说明，避免花屏。
// ---------------------------------------------------------------------------
struct BoardProfile {
  const char* name;

  // LVGL 逻辑画布（UI 设计分辨率）
  int width;
  int height;

  // 面板/显存尺寸（LovyanGFX 声明尺寸）
  int panelWidth;
  int panelHeight;

  // 驱动旋转 / 镜像位 / 刷新层旋转（0=不转，90/270=横屏）
  int rotation;
  int offsetRotation;
  int flushRotation;

  // SPI 引脚，-1 表示悬空/不使用
  int8_t pinSck;
  int8_t pinMosi;
  int8_t pinMiso;
  int8_t pinCs;
  int8_t pinDc;
  int8_t pinRst;
  int8_t pinBl;

  // 与 LCD 共用 SPI 总线、启动时需拉高禁用的片选（-1 = 无）
  int8_t pinTfCs;
  int8_t pinTouchCs;

  uint32_t spiHz;

  bool invert;    // 面板反色
  bool rgbOrder;  // true = RGB，false = BGR

  bool driverIli9341;  // true = ILI9341，false = ST7789

  uint8_t bufferLines;  // LVGL 局部刷新缓冲行数（越大越平滑，越费 RAM）
};
