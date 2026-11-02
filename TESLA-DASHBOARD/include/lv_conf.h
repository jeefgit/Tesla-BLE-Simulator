/**
 * LVGL 9 配置（PlatformIO 构建参数 -DLV_CONF_INCLUDE_SIMPLE 使 lvgl 直接 include 本文件）
 * 只覆盖与默认不同的项；未列出的项用 lv_conf_internal.h 默认值。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stddef.h>
#include <stdint.h>

/*-------------------
 * 显存/颜色
 *--------------------*/
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565 /* RGB565，匹配 ST7789 */
#define LV_COLOR_16_SWAP 0           /* ESP32 与 ST7789 都是小端，不交换 */
/* LVGL 对象池：64KB 整块从 ESP32 堆申请，不再占静态 .dram0.bss
 * （静态段只剩几 KB，LV_MEM_SIZE 设 64KB 会 region dram0_0_seg overflowed）。
 *
 * 关键约束：TLSF 的「单个池上限」= LV_MEM_SIZE（lv_tlsf.c 用
 * TLSF_LOG2_CEIL(LV_MEM_SIZE) 算 block_size_max），所以
 *   · LV_MEM_SIZE 必须 ≥ 实际池大小；
 *   · 用 lv_mem_add_pool() 追加比 LV_MEM_SIZE 大的池会被静默拒绝（返回 0）。
 * 这里改为让 LVGL 自己按 LV_MEM_SIZE 建池，内存由 lvglPoolAlloc() 提供
 * （heap_caps_aligned_alloc，16 字节对齐，见 Display.cpp）。 */
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_POOL_ALLOC(size) lvglPoolAlloc(size)

/*-------------------
 * 渲染节奏
 *--------------------*/
#define LV_DEF_REFR_PERIOD 33        /* ~30fps 刷新 */
#define LV_INDEV_DEF_READ_PERIOD 30  /* 无触摸，仅影响指针设备轮询 */

/*-------------------
 * 字体（只启用用到的，省 Flash）
 *   48 = 车速   26 = SOC / 充电功率   16 = 续航/告警
 *   14 = 档位/温度/锁   12 = 胎压/单位   10 = BLE 状态   8 = 区域小标签
 *--------------------*/
#define LV_FONT_MONTSERRAT_8 1    /* 区域小标签/轮位（对应设计稿 8px） */
#define LV_FONT_MONTSERRAT_10 1   /* BLE 状态（对应设计稿 9.5px） */
#define LV_FONT_MONTSERRAT_12 1   /* 胎压数值/单位（对应设计稿 10.5–12px） */
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

/*-------------------
 * 断言：LVGL 默认全关，内存不足时 malloc 返回 NULL 也不检查，
 * 直接写 NULL+offset → Guru Meditation（EXCVADDR 0x8）看不出原因。
 * 这里打开 malloc/null 检查，并换成「打串口 + 重启」的处理器。
 *--------------------*/
#ifdef __cplusplus
extern "C" {
#endif
void lvglAssertFailed(const char* file, int line);
void* lvglPoolAlloc(size_t size);
#ifdef __cplusplus
}
#endif
#define LV_ASSERT_HANDLER lvglAssertFailed(__FILE__, __LINE__);
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_NULL 1

/* 日志：屏幕开发期关掉，串口只看我们自己的 CFG_LOG */
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
