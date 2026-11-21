# v2026.11.22.0008 改动说明

## TeslaBleBackend 拆分为独立仓库

`TeslaBleBackend` 库从本仓库拆分，迁移至独立仓库：`https://github.com/jeefgit/TeslaBleBackend`

### 主要变更

- 删除仓库根目录下的 `TeslaBleBackend/`（原为 PlatformIO 库，供 ESP32 仪表工程通过 `lib_extra_dirs = ..` 引用）
- `ESP32-S3-ROUND-DISPLAY/platformio.ini`：
  - 移除 `lib_extra_dirs = ..` 及配套的 `lib_ignore` 配置
  - `lib_deps` 改为直接引用 `https://github.com/jeefgit/TeslaBleBackend.git`，由 PlatformIO 从 GitHub 拉取

### 功能影响

- 车端 BLE 数据源、加密握手与配钥逻辑无任何代码改动，本固件与上一版（v2026.11.21.2332）的运行行为一致
- 库源码后续在独立仓库演进，本仓库仅记录引用版本

### 相关提交

- 新库仓库：`7eb2ff3` 初始提交、`536c6c0` 修正 library.json 仓库地址
- 本仓库：`83df73d` 移除 TeslaBleBackend 并改用 lib_deps 引用
