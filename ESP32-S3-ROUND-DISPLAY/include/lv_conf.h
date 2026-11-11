/**
 * LVGL v9 配置（工程私有）。
 *
 * 以前这些宏写在 platformio.ini 的 build_flags 里，任何一次微调都会改变
 * 所有文件的编译命令，导致工程、LVGL、Arduino_GFX、Arduino core 全部重编。
 * 放到这里以后，只有 LVGL 自身会因头文件变化而重编，platformio.ini 保持稳定。
 *
 * 只写与默认值不同的项；其余选项由 lv_conf_internal.h 提供默认值。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 用 newlib 的 malloc/free，而不是 LVGL 自带的 96KB 内置堆。
 *
 * 原因：带 transform（缩放/旋转）的对象需要一个 ARGB8888 图层缓冲，
 * 尺寸按对象框计算。速度数字标签是 466x56，约 120KB，内置堆 96KB 放不下，
 * lv_malloc 返回 NULL，而 LVGL 9.6 在变换路径上没有判空，直接写 0 地址，
 * 表现为 StoreProhibited / EXCVADDR 0x0。
 *
 * 换成 CLIB 后走 ESP-IDF 的堆分配器：优先内部 RAM，不够时自动落到 PSRAM，
 * 120KB 的图层也能分配成功。Waveshare 的 ESP-IDF 示例同样是 CLIB malloc。
 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

/* 仪表用到的字体。 */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_H */

