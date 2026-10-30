/**
 * LVGL 9 配置（PlatformIO 构建参数 -DLV_CONF_INCLUDE_SIMPLE 使 lvgl 直接 include 本文件）
 * 只覆盖与默认不同的项；未列出的项用 lv_conf_internal.h 默认值。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*-------------------
 * 显存/颜色
 *--------------------*/
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565 /* RGB565，匹配 ST7789 */
#define LV_COLOR_16_SWAP 0           /* ESP32 与 ST7789 都是小端，不交换 */
#define LV_MEM_SIZE (24U * 1024U)     /* LVGL 对象堆 24KB（对象树 <100 个足够；BLE 模式静态 RAM 紧张，从 32KB 调低省 8KB） */

/*-------------------
 * 渲染节奏
 *--------------------*/
#define LV_DEF_REFR_PERIOD 33        /* ~30fps 刷新 */
#define LV_INDEV_DEF_READ_PERIOD 30  /* 无触摸，仅影响指针设备轮询 */

/*-------------------
 * 字体（只启用用到的，省 Flash）
 *   56 = 车速   40 = 充电功率   26 = SOC
 *   20 = 续航   16 = 档位/温度/单位/告警   14 = 区域标签/胎压位号
 *--------------------*/
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_48 1   /* 内置最大号，车速大字用（无 56） */
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/*-------------------
 * 控件（仪表盘用到的）
 *--------------------*/
#define LV_USE_LABEL 1
#define LV_USE_IMG 0
#define LV_USE_BAR 0
#define LV_USE_ARC 0
#define LV_USE_CANVAS 0
#define LV_USE_LINE 0
#define LV_USE_ROLLER 0
#define LV_USE_TEXTAREA 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LIST 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_WIN 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_OBJSX 0
#define LV_USE_SWITCH 0
#define LV_USE_SLIDER 0
#define LV_USE_SCALE 0   /* 依赖 lv_line，不用则一并关 */

/* 符号字体（告警条 ⚠ 等） */
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_FONT_MONTSERRAT_14 1

/* 日志：屏幕开发期关掉，串口只看我们自己的 CFG_LOG */
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
