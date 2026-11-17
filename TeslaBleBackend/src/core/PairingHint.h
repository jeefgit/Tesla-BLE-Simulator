#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// PairingHint：配钥流程的「屏幕提示」状态
//
// 由数据源生产、UI 消费，让用户不用盯串口日志也知道现在该干什么：
//   1. add-key-request 发出后：提示贴卡 + 车端刷卡窗口倒计时
//   2. 车端回 WAIT：提示在屏幕上确认
//   3. info=25/26 超时：提示正在自动重试
//   4. 成功 / 失败：给出终态
//
// 屏幕字体只有 Montserrat（ASCII），所以界面文案用英文；串口日志仍是中文。
// ---------------------------------------------------------------------------
struct PairingHint {
  enum Phase : uint8_t {
    Idle = 0,     // 没在配钥（屏幕上不显示任何配钥提示）
    TapCard,      // 已发 add-key-request（含车端 WAIT），等贴卡；贴卡到车机确认完成前都停在这里
    WaitConfirm,  // 保留：车端读到卡、等待中控屏确认（当前固件不再进入，避免界面来回跳）
    Retry,        // 超时或暂时失败，固件会自动重来
    Success,      // 配钥成功且加密握手已建立（短时展示）
    Failed,       // 需要人工处理（车主拒绝 / 白名单满等）
  };

  Phase phase = Idle;
  uint32_t remainMs = 0;  // 车端刷卡窗口剩余时间（TapCard/WaitConfirm 有效）
  uint32_t info = 0;      // 车端 WhitelistOperation_status.info
  uint8_t attempt = 0;    // 当前是第几次 add-key-request
  uint8_t maxAttempts = 0;  // 一轮最多发几次
};
