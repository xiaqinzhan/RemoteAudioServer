// app.js — 全局逻辑，持有服务器地址配置
App({
  globalData: {
    // 默认云端服务器地址（发布前请在微信公众平台配置 request/socket 合法域名）
    // 局域网调试时改为 "http://192.168.x.x:8000"
    serverBase: 'https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site'
  },
  onLaunch() {}
});
