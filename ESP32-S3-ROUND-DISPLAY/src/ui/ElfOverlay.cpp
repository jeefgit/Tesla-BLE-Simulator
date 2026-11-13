#include "ui/ElfOverlay.h"

#include <Arduino.h>

#include "config.h"

#if CFG_ELF_ENABLE

namespace {

// 全部坐标直接使用 466×466 的 LVGL 画面坐标（见 design/ELF-DESIGN.md §3.2）。
constexpr int32_t kScreenSize = 466;

const lv_color_t kElfBg = lv_color_hex(0x020406);
const lv_color_t kSclera = lv_color_hex(0xE8F7FF);
const lv_color_t kIris = lv_color_hex(0x1C4E63);
const lv_color_t kPupil = lv_color_hex(0x04070A);
const lv_color_t kGlow = lv_color_hex(0x43D9F5);
const lv_color_t kBlush = lv_color_hex(0xFF8A80);
const lv_color_t kSleep = lv_color_hex(0x5A6377);

constexpr int32_t kEyeW = 128;
constexpr int32_t kEyeH = 150;
constexpr int32_t kEyeY = 133;
constexpr int32_t kEyeX[2] = {90, 248};
constexpr int32_t kBlushX[2] = {103, 329};  // 中心 120 / 346，尺寸 34×16
constexpr int32_t kIrisSize = 92;
constexpr int32_t kIrisInset = (kEyeW - kIrisSize) / 2;  // 18
constexpr int32_t kIrisY = (kEyeH - kIrisSize) / 2;      // 29
constexpr int32_t kPupilSize = 74;
constexpr int32_t kPupilInset = (kIrisSize - kPupilSize) / 2;  // 9

// 建一个纯色几何块：无边框、不可点击、不滚动、不参与布局。
lv_obj_t* makeBlock(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                    int32_t h, int32_t radius, lv_color_t color, uint8_t opa,
                    bool clipCorner = false) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_bg_color(obj, color, 0);
  lv_obj_set_style_bg_opa(obj, opa, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_clip_corner(obj, clipCorner, 0);
  lv_obj_set_scrollable(obj, false);
  lv_obj_set_clickable(obj, false);
  return obj;
}

}  // namespace

void ElfOverlay::begin() {
  if (m_built) {
    return;
  }
  buildObjects();
  m_built = true;
  CFG_LOG("[ELF] overlay built, heap %u", ESP.getFreeHeap());
}

void ElfOverlay::buildObjects() {
  lv_obj_t* screen = lv_screen_active();

  // 根容器：不透明纯黑（AMOLED 关闭像素），盖住下面的仪表页；
  // 不可点击、默认隐藏。
  m_root = makeBlock(screen, 0, 0, kScreenSize, kScreenSize, 0, kElfBg,
                     LV_OPA_COVER);
  lv_obj_add_flag(m_root, LV_OBJ_FLAG_HIDDEN);

  // 装饰弧：R210、1px、8% 青色，用大圆边框代替阴影。
  m_aura = makeBlock(m_root, 23, 23, 420, 420, LV_RADIUS_CIRCLE,
                     lv_color_black(), LV_OPA_TRANSP);
  lv_obj_set_style_border_width(m_aura, 1, 0);
  lv_obj_set_style_border_color(m_aura, kGlow, 0);
  lv_obj_set_style_border_opa(m_aura, 20, 0);  // 约 8%

  for (int i = 0; i < 2; ++i) {
    // 眼球：胶囊形，子对象随高度压缩被裁掉，形成“合眼”。
    m_eye[i] = makeBlock(m_root, kEyeX[i], kEyeY, kEyeW, kEyeH, 64, kSclera,
                         LV_OPA_COVER, true);

    m_iris[i] = makeBlock(m_eye[i], kIrisInset, kIrisY, kIrisSize, kIrisSize,
                          LV_RADIUS_CIRCLE, kIris, LV_OPA_COVER);
    m_pupil[i] = makeBlock(m_iris[i], kPupilInset, kPupilInset, kPupilSize,
                           kPupilSize, LV_RADIUS_CIRCLE, kPupil, LV_OPA_COVER);
    // 高光挂在虹膜下：瞳孔放大时位置稳定，不会跟着漂。
    makeBlock(m_iris[i], 15, 15, 26, 26, LV_RADIUS_CIRCLE, lv_color_white(),
              230);
    makeBlock(m_iris[i], 58, 56, 12, 12, LV_RADIUS_CIRCLE, lv_color_white(),
              140);

    // 腮红：低不透明度胶囊，默认几乎看不见。
    m_blush[i] = makeBlock(m_root, kBlushX[i], 298, 34, 16, 8, kBlush, 31);
  }

  // 嘴：默认隐藏，开心时再出现。
  m_mouth = makeBlock(m_root, 211, 342, 44, 14, 7, kSleep, 77);
  lv_obj_add_flag(m_mouth, LV_OBJ_FLAG_HIDDEN);
}

bool ElfOverlay::visible() const { return m_visible; }

void ElfOverlay::show() {
  if (!m_built || m_visible) {
    return;
  }
  lv_obj_clear_flag(m_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(m_root);
  m_visible = true;
  CFG_LOG("[ELF] show");
}

void ElfOverlay::hide() {
  if (!m_built || !m_visible) {
    return;
  }
  lv_obj_add_flag(m_root, LV_OBJ_FLAG_HIDDEN);
  m_visible = false;
  CFG_LOG("[ELF] hide");
}

void ElfOverlay::lookAt(int16_t screenX, int16_t screenY) {
  // 阶段 3 实现：眼睛追随手指。
  (void)screenX;
  (void)screenY;
}

void ElfOverlay::poke(int16_t screenX, int16_t screenY) {
  // 阶段 3 实现：开心反应。
  (void)screenX;
  (void)screenY;
}

#else  // CFG_ELF_ENABLE == 0

void ElfOverlay::begin() {}
bool ElfOverlay::visible() const { return false; }
void ElfOverlay::show() {}
void ElfOverlay::hide() {}
void ElfOverlay::lookAt(int16_t, int16_t) {}
void ElfOverlay::poke(int16_t, int16_t) {}

#endif  // CFG_ELF_ENABLE
