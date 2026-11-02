# LVGL 内存问题排查记录

> 背景：主数据页整体重排后（对象树从 ≈60 个涨到 ≈78 个），真机连续三次崩溃，
> 最后确认是 **LVGL 对象池（TLSF）容量不足 + 池扩容方式无效**。
> 本文记录现象、定位方法、根因和最终配置，避免以后再踩。

---

## 1. 现象

串口表现为 `Guru Meditation Error`，两次的异常类型还不一样：

| 触发时机 | 异常 | EXCVADDR | 关键 backtrace |
| :--- | :--- | :--- | :--- |
| 首帧渲染 | `StoreProhibited` | `0x00000008` | `lv_draw_add_task`（`lv_draw.c:100`） |
| 构建对象树 | `LoadProhibited` | `0x00000008` | `lv_obj_add_style`（`lv_obj_style.c:153`） |

两个地址都落在 **`NULL + 8`**：LVGL 的 `lv_malloc()` / `lv_realloc()` 返回 NULL 后
**不做检查**（`LV_USE_ASSERT_MALLOC` 默认关闭），下一行就往结构体偏移 8 的成员写/读。

---

## 2. 定位方法

串口日志里的地址是绝对地址，用 ELF + addr2line 反查（注意工具链路径是
`toolchain-xtensa-esp-elf`，不是 `-esp32s3`）：

```powershell
cd TESLA-DASHBOARD
& "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp32-elf-addr2line.exe" `
  -pfiaC -e ".pio\build\esp32dev\firmware.elf" `
  0x40090a08 0x40142174 0x40161e66 0x401625f9 ...
```

输出会直接给到 `文件:行`，两次都指向「malloc 结果未检查」的位置，而不是业务逻辑。

---

## 3. 根因（四层，逐层踩出来的）

### 3.1 池容量：24KB 静态池不够

重排后对象 ≈78 个（含 6 门、4 轮、4 组胎压块、车身中线/前后风挡等），
平均每对象含样式属性约 300–500B，光对象树就要 25–40KB，24KB 池首帧就爆。

### 3.2 不能简单调大 `LV_MEM_SIZE`

`LV_MEM_SIZE` 默认对应一块 **静态数组**（`lv_mem_core_builtin.c` 里的
`work_mem_int[]`）。本项目静态段已接近 `dram0_0_seg` 上限（显示双缓冲 30KB +
NimBLE 静态数据等），改成 48KB 直接链接失败：

```
region `dram0_0_seg' overflowed by 18760 bytes
```

### 3.3 `lv_mem_add_pool()` 追加池：被静默拒绝

第一版尝试：静态 `LV_MEM_SIZE` 降到 8KB，启动后 `lv_mem_add_pool()` 补 40–56KB。
真机日志：

```
错误: LVGL 堆扩容失败; LVGL 堆: 共 7KB（静态 8KB + 堆 0KB）,
ESP 空闲 157660 B / 最大块 110580 B
```

堆里有 157KB，说明不是申请不到，而是 **TLSF 拒绝了**。
`lv_tlsf.c` 的**单池上限由 `LV_MEM_SIZE` 决定**：

```c
#define TLSF_MAX_POOL_SIZE (LV_MEM_SIZE)
FL_INDEX_MAX   = TLSF_LOG2_CEIL(TLSF_MAX_POOL_SIZE);
block_size_max = 1 << FL_INDEX_MAX;
...
if (pool_bytes < block_size_min || pool_bytes >= block_size_max) return 0;  /* 静默返回 0 */
```

`LV_MEM_SIZE = 8KB` → `block_size_max = 8KB`，追加 56KB 的池必然被拒；
`LV_USE_LOG = 0` 时连那行警告都看不到，所以“加了等于没加”。

### 3.4 对齐：TLSF 还要求池首地址 8 字节对齐

```c
if (((ptrdiff_t)mem % ALIGN_SIZE) != 0) return 0;   /* ALIGN_SIZE = 8 */
```

ESP32 的 `malloc()` 只保证 4 字节，必须用 `heap_caps_aligned_alloc()`。

---

## 4. 最终方案（当前代码）

`include/lv_conf.h`：

```c
#define LV_MEM_SIZE (64U * 1024U)                    /* 同时也是 TLSF 单池上限 */
#define LV_MEM_POOL_ALLOC(size) lvglPoolAlloc(size)  /* 池内存从 ESP32 堆来，不占静态段 */

#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_NULL 1
#define LV_ASSERT_HANDLER lvglAssertFailed(__FILE__, __LINE__);
```

`src/display/Display.cpp`：

```cpp
extern "C" void* lvglPoolAlloc(size_t size) {
  void* p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);  // 16B 对齐
  if (!p) { 打日志 + esp_restart(); }                            // 不再静默写 NULL
  return p;
}
```

要点：

1. **让 LVGL 自己按 `LV_MEM_SIZE` 建池**（`lv_init()` → `lv_mem_init()`），
   单池上限与实际池大小一致，不再有“单池上限 < 池大小”的矛盾。
2. 池内存由堆提供，静态 RAM 反而更省：**28.7%（94184 B）**，比最初 24KB 静态池时低 24KB。
3. 断言处理器把 `Guru Meditation` 换成可读日志：

   ```
   错误: LVGL 断言失败 (…/lv_obj_style.c:153) —— 内存不足或参数非法，重启
   ```

---

## 5. 运行期自检

启动日志（`Display::begin` / `Dashboard::begin`）：

```
LVGL 堆: 共 64KB, ESP 空闲 157660 B / 最大块 110580 B
仪表层就绪（LVGL 对象树, 320x240）: 堆 46% 已用 (30116/65536 B), 峰值 41220 B
```

`Display::loop()` 每 30s 采样 `lv_mem_monitor()`，`used_pct ≥ 85%` 时打警告：

```
警告: LVGL 堆: 88% 已用 (57672/65536 B), 峰值 61200 B, 碎片 71% | ESP 空闲 … B, 最低 … B
```

排查顺序：

1. 先看第一行的 **共 xxKB** 是不是 64KB（不是就说明池没建起来）。
2. 再看对象树建完时的 **已用/峰值**：占 85% 以上就该加池或减对象。
3. 看 `ESP 空闲 / 最低`：LVGL 池加大后若 BLE 起不来，就是挤占了 NimBLE 的堆。

---

## 6. 下次改 UI 的注意事项

- **不要再把 `LV_MEM_SIZE` 调回静态小值再追加池**——除非单池上限也跟着改，否则追加永远失败。
- 加对象（尤其是带多组 `lv_obj_set_style_*` 的容器）前，先估一下：本项目约 **300–500B/对象**。
- `LV_USE_LOG = 0` 会让 LVGL 内部的 `LV_LOG_WARN/ERROR`（包括 TLSF 的拒绝原因）全部消失；
  排查内存问题时临时打开，比猜快得多。
- 断言一定要开（`LV_USE_ASSERT_MALLOC/NULL`），否则 OOM 只会表现为随机地址的 Guru Meditation。
