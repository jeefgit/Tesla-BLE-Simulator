// 固件版本号唯一实现。请勿把 fw_version.h 包含到其它源文件——需要版本号的地方
// 通过 include/fw_version_api.h 里的 fwVersion() 获取，以保证每次改版本号
// 只重编本文件一个编译单元。
#include "fw_version.h"

const char* fwVersion() {
  return CFG_FW_VERSION;
}
