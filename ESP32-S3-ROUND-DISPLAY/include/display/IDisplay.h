#pragma once

class IDisplay {
 public:
  virtual ~IDisplay() = default;
  virtual bool begin() = 0;
  virtual void loop() = 0;
};
