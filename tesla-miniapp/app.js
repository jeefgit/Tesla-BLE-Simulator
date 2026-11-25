// Tesla BLE 仪表小程序 — 应用入口
const config = require('./config/index');

App({
  onLaunch() {
    // 全局配置挂到 app 上，页面通过 getApp().globalData 读取
    this.globalData.config = config;
  },

  globalData: {
    config: null,
  },
});
