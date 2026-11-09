#pragma once

#include "core/UiModel.h"

// 圆屏 UI 的数据源接口：实现负责把底层车况映射成 UiModel。
// 与 TeslaBleBackend 的 IVehicleSource 分开命名，避免同名类/头文件冲突。
class IUiModelSource {
 public:
  virtual ~IUiModelSource() = default;
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual const UiModel& model() const = 0;
};
