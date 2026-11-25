// 组帧器：字节流 → 完整帧
// - plain 模式：53 字节裸 SimPayload（帧长==53 即完整）
// - encrypted 模式：2 字节大端长度前缀帧（declared + 2 == 已收字节数 自洽即完整）
// 支持跨通知边界任意分块；encrypted 模式下 53B 形状的噪声帧直接丢弃（硬约束）
'use strict';

const { CFG, SIM_PAYLOAD_LEN } = require('../config/index');

const MODE_PLAIN = 'plain';
const MODE_ENCRYPTED = 'encrypted';

class FrameParser {
  constructor() {
    this._buf = new Uint8Array(0);
    this._mode = MODE_PLAIN;
    this._handshakeDone = false;
    this._dropped = 0;
  }

  get mode() { return this._mode; }

  // 握手成功前置 false：53B 形状帧按旁路噪声丢弃；置 true 后按合法加密帧处理
  setHandshakeDone(v) {
    this._handshakeDone = !!v;
  }

  setMode(mode) {
    if (mode !== MODE_PLAIN && mode !== MODE_ENCRYPTED) return;
    if (mode !== this._mode) {
      this._mode = mode;
      this.reset(); // 模式切换时旧缓冲语义失效
    }
  }

  reset() {
    this._buf = new Uint8Array(0);
  }

  get buffered() { return this._buf.length; }

  // 输入一段原始字节，返回完整帧数组：[{type:'sim'|'enc', data:Uint8Array}]
  feed(bytes) {
    if (!bytes || bytes.length === 0) return [];
    const merged = new Uint8Array(this._buf.length + bytes.length);
    merged.set(this._buf);
    merged.set(bytes, this._buf.length);
    this._buf = merged;

    const frames = [];
    for (;;) {
      const n = this._buf.length;
      if (n === 0) break;

      if (this._mode === MODE_PLAIN) {
        if (n >= SIM_PAYLOAD_LEN) {
          frames.push({ type: 'sim', data: this._buf.subarray(0, SIM_PAYLOAD_LEN) });
          this._buf = this._buf.slice(SIM_PAYLOAD_LEN);
          continue;
        }
        break; // 不满 53B，等下一个通知块
      }

      // encrypted 模式：流式长度前缀提取（支持单块多帧 / 跨块重组）
      if (n >= 2) {
        const declared = (this._buf[0] << 8) | this._buf[1];
        const total = declared + 2;
        if (declared === SIM_PAYLOAD_LEN - 2 && !this._handshakeDone) {
          // 53B 形状帧：握手成功前视为旁路噪声（硬约束），直接丢弃
          if (n >= SIM_PAYLOAD_LEN) {
            this._dropped++;
            this.emitDropped(this._buf.subarray(0, SIM_PAYLOAD_LEN));
            this._buf = this._buf.slice(SIM_PAYLOAD_LEN);
            continue;
          }
          break;
        }
        if (declared > CFG.MAX_FRAME) {
          // 前缀非法：丢 1 字节重新同步
          this._dropped++;
          this._buf = this._buf.slice(1);
          continue;
        }
        if (n < total) break; // 帧未收全
        if (declared === 0) {
          // 空帧异常，跳过 2 字节前缀
          this._buf = this._buf.slice(2);
          continue;
        }
        frames.push({ type: 'enc', data: this._buf.subarray(2, total) });
        this._buf = this._buf.slice(total);
        continue;
      }
      break;
    }
    if (this._buf.length > CFG.MAX_FRAME) {
      this._dropped++;
      this.reset();
    }
    return frames;
  }

  emitDropped(bytes) {
    // 保留钩子：调试时可订阅 dropped 事件观察旁路噪声
    this.onDropped && this.onDropped(bytes);
  }
}

module.exports = { FrameParser, MODE_PLAIN, MODE_ENCRYPTED };
