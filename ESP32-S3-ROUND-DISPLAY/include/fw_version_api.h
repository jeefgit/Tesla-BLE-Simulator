// 固件版本号访问接口。
//
// 版本号的“值”由编译脚本生成在 include/fw_version.h（见 scripts/bump_version.py），
// 但业务代码不要直接 include 那个文件——那样每次改版本号都会连带重编所有使用方。
// 统一 include 本头文件并调用 fwVersion() 即可：实现位于 src/fw_version.cpp，
// 它是唯一 include fw_version.h 的源文件，版本号变化时只重编它一个。
#pragma once

const char* fwVersion();
