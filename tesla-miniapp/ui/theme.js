// 仪表配色 token（与 dashboard.wxss 完全对齐的小程序浅色 Tesla 色板）
'use strict';

module.exports = {
  SCREEN: '#FFFFFF',        // Canvas 背景
  TEXT: '#2C3E50',          // 主文本
  DIM: '#8B98A8',           // 次要文本
  FAINT: '#B2B8C0',         // 弱化/占位
  TRACK: '#EFF1F5',         // 圆环底轨

  GAUGE_CYAN: '#10AEFF',    // Tesla 蓝：速度/信息
  TESLA_RED: '#FA5151',     // 微信红：功率输出/告警
  REGEN_GREEN: '#07C160',   // 微信绿：充电/回收/成功
  CAUTION: '#FFA033',       // 速度中段过渡
  ALERT: '#FA5151',

  FONT: 'sans-serif',
};
