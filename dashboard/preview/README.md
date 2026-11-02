# 仪表盘界面预览（320×240）

这里放的是**和固件实机布局对齐的静态预览图**，用于改稿评审、issue 贴图和
README 展示，不用反复烧板。

| 文件 | 说明 |
| :--- | :--- |
| `dashboard_states.png` | 4 状态横排：正常行驶 / 车门+胎压+低电告警 / 充电 / 断连扫描 |
| `dashboard_normal_2x.png` | 正常行驶状态 2 倍放大（看对齐细节） |
| `mock_render.py` | 生成上面两张图的脚本（Pillow，只依赖系统字体） |

## 重新生成

```bash
pip install pillow
python dashboard/preview/mock_render.py
```

脚本里的坐标/配色和 `TESLA-DASHBOARD/src/app/Dashboard.cpp`、
`dashboard/DESIGN.md` 是同一套，**改布局时三处一起改**。

## 注意

- 字体用系统 Arial 近似 LVGL 内置的 Montserrat，字宽和圆角会有 1–2px 差异；
  以实机截图为准。
- 这是"设计意图"预览，不参与编译，也不影响固件体积。
- 另外一个交互原型 `../ui-mockup.html` 可以在浏览器里切换 7 种状态并缩放，
  适合讨论交互，不适合贴到文档里。
