# TeslaBleBackend

PlatformIO library for Tesla BLE vehicle data on **ESP32 with the Arduino
framework**. It owns the NimBLE client, Tesla framing/crypto/pairing logic, and
the vehicle snapshot interface. It does not depend on the dashboard, LVGL,
LovyanGFX, or a particular screen.

## Install in another project

Copy this directory into the consumer project's `lib/TeslaBleBackend/`, or add
the parent `lib` directory to `lib_extra_dirs`. Add the BLE dependency:

```ini
[env:your-esp32]
platform = espressif32
framework = arduino
lib_deps =
    h2zero/NimBLE-Arduino@^2.5.1
```

Provide `include/tesla_backend_user_config.h` in the consumer project:

```cpp
#pragma once
#define CFG_TESLA_VIN "YOUR_VIN"
#define CFG_TESLA_CLIENT_KEY_HEX "YOUR_64_HEX_CHAR_PRIVATE_KEY"
```

The private key must remain local and must not be committed. Optional settings
such as GATT UUIDs, poll intervals, unit conversion, and logging can also be
overridden in this header. When the header is absent, protocol defaults are
used and diagnostic logging is disabled.

## API

```cpp
#include <TeslaBleBackend.h>

TeslaBleBackend vehicle;

void setup() {
  vehicle.begin();
}

void loop() {
  vehicle.loop();
  const VehicleState& state = vehicle.state();
  const SourceStatus status = vehicle.status();
}
```

Call `loop()` frequently from the Arduino main loop. `state()` and `status()`
return read-only snapshots suitable for any board-specific UI or telemetry
consumer.

This library is currently kept as a subdirectory of the simulator repository.
It is not yet a standalone PlatformIO Registry package or Git repository.
