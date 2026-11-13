#pragma once

#include <lvgl.h>
#include <stdint.h>

// 离线小精灵浮层（见 design/ELF-DESIGN.md）。
//
// 对象树在 begin() 时一次性建好，之后只改属性；默认隐藏。
// 由 DashboardApp 负责手势识别，本类只负责显隐与动画。
class ElfOverlay {
 public:
  // 在 active screen 上建对象；必须在 LvglDashboard::begin() 之后调用，
  // 否则会被开机图层盖住。
  void begin();

  bool visible() const;

  // 左滑 / 右滑调用。
  void show();
  void hide();

  // 触摸移动：眼睛追随手指（屏幕坐标已换算成 LVGL 画面坐标）。
  void lookAt(int16_t screenX, int16_t screenY);

  // 轻点：开心反应 + 重置自动隐藏计时。
  void poke(int16_t screenX, int16_t screenY);

 private:
  void buildObjects();

  lv_obj_t* m_root = nullptr;
  lv_obj_t* m_aura = nullptr;
  lv_obj_t* m_eye[2] = {};
  lv_obj_t* m_iris[2] = {};
  lv_obj_t* m_pupil[2] = {};
  lv_obj_t* m_blush[2] = {};
  lv_obj_t* m_mouth = nullptr;
  bool m_built = false;
  bool m_visible = false;
};
