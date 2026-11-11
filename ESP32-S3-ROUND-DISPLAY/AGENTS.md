# 工程约定

## 不要修改 `platformio.ini`

用户明确要求：**以后尽量不要修改 `ESP32-S3-ROUND-DISPLAY/platformio.ini`**。

原因：这个文件里的 `build_flags` / `board_*` 一旦变动，会触发 LVGL（约 550 个文件）、
Arduino_GFX、Arduino core 的全量重编，单次约 10~17 分钟。

需要调参时改这些文件：

- LVGL 功能/字体/内存策略：`include/lv_conf.h`
- 业务开关（胎压、车门、demo 模式等）：`include/config.h`
- 显示驱动参数：`src/display/RoundDisplay.cpp`、`include/platform/boards/*`

只有用户明确同意时才改 `platformio.ini`。
