# XiaoZhi111 项目记忆

- 用户在开发基于 xiaozhi-esp32 的自定义板 "AI-dual-eyed-doll"（日志固件版本 2.4.0，双麦克风 AFE 2MIC，配网走 softAP + web）。
- 唤醒词模型两条官方加载路径（v2.4.0）：① assets 分区（v2 分区表默认，8M SPIFFS，取代 v1 的 960KB model 分区，内容可 OTA）→ LoadSrmodelsFromIndex → srmodel_load → AudioService::SetModelsList；② model 分区（esp-sr CMake 打包 srmodels.bin 并由 idf.py flash 烧录）→ esp_srmodel_init("model")。两路都断则 AFE detector: none，唤醒完全失效，且构建期不报错。
- 配网后重连：esp-wifi-connect station 先全信道扫描，失败按 10s→20s→… 指数退避；APSTA→STA 切换后立即扫描易返回空。3.3.x 起改为已保存信道优先快扫。
- 官方仓库：78/xiaozhi-esp32；WiFi 组件：78/esp-wifi-connect；模型：espressif/esp-sr（唤醒词菜单可选，中文默认「你好小智」wn9s_nihaoxiaozhi）。
