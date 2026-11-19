#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vinstore {

constexpr size_t kLength = 17;
constexpr size_t kCapacity = kLength + 1;

enum class Error : uint8_t {
  None = 0,
  Empty,
  Length,
  InvalidChar,
  StrictIso,
  NvsWrite,
};

// 启动时调用一次：加载 NVS 覆盖值，非法则回退 CFG_TESLA_VIN。
void begin();

// 当前生效 VIN；可能为空字符串（配置也无效且 NVS 无值）。
const char* current();

// 当前是否没有 NVS 覆盖（即使用出厂默认值）。
bool isDefault();

// 纯校验：长度 17、A-Z0-9，可选 I/O/Q 检查。
Error validate(const char* vin);

// 大写规范化 + 校验。out 必须至少 kCapacity 字节。
Error normalize(const char* in, char out[kCapacity]);

// 校验并写 NVS，成功后同步更新 current()。
Error save(const char* vin);

// 删除 NVS 覆盖，恢复出厂默认值。
bool clear();

// 给 UI 用的英文短提示，例如 "VIN MUST BE 17 CHARACTERS"。
const char* errorText(Error error);

}  // namespace vinstore
