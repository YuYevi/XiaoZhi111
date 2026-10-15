# AI 双眼玩偶

本目录是 AI 双眼玩偶的独立板级实现。该硬件与 Waveshare ESP32-S3-Touch-LCD-1.85 不兼容，因此使用独立的板卡标识。

## 硬件

- 主控：ESP32-S3R8，4 MB Flash，使用八线 PSRAM。
- 音频：ES8390（复用 ES8389 驱动）、LM4890 功放，双路模拟麦克风输入。
- 显示：两块 0.9 英寸 BOE 圆形 ST7735S 屏幕，分辨率 128x128，SPI 总线共享时钟、数据、DC 和复位信号，分别使用 GPIO12/GPIO13 片选。
- 电源：GPIO2 为电源键，GPIO1 控制保持供电，GPIO16 检测充电状态。
- 触摸：GPIO8 和 GPIO38 为 TTP233H 触摸输入，目前未绑定功能。
- 注意：GPIO33-37、GPIO47-48 被八线 PSRAM 占用，不要复用。

屏幕通过 ESP-IDF 的 ST7789 驱动初始化。ST7735S 与本项目使用的显示命令兼容，BOE 面板额外的电源、帧率、VCOM 和伽马参数在驱动初始化后直接发送。屏幕使用 RGB565、偏移量 `(2, 2)` 和颜色反转，右眼通过镜像参数适配实际安装方向。

## 按键功能

| 输入 | GPIO | 功能 |
| --- | --- | --- |
| KEY 1 短按 | 17 | 循环播放 `shckblnk`、`cuteblnk`、`hapyblnk` 动画及对应语音 |
| KEY 1 长按 | 17 | 开关场景语音；重新打开时播放 `tsundere.ogg` 提示 |
| 电源键短按 | 2 | 循环播放 `cute`、`happy`、`sad` 动画及对应语音 |
| 电源键长按 | 2 | 关机；开机时需持续按住约 3 秒 |
| TOUCH 1/2 | 8/38 | 当前未使用 |
| BOOT | 0 | 运行时未使用 |

每次场景切换都会从第一帧重新播放动画和语音。动画会持续循环，语音结束不会停止动画。语音播放由独立任务负责，不会阻塞主事件循环。KEY 1 不再切换 Wi-Fi 配网 AP；首次启动且没有已保存 Wi-Fi 时，原有配网流程保持不变。

## 资源

静态表情位于 `assets/`，以 RGB565 压缩数据嵌入固件。动态眼睛动画位于 `partition_assets/`，构建时写入 1 MB 的只读 `assets` 分区。动画文件名最多使用 15 个字符，这是 `CONFIG_MMAP_FILE_NAME_LENGTH=16` 的限制。

当前动画如下：

| 文件 | 用途 | 帧间隔 |
| --- | --- | --- |
| `shckblnk.anim.z` | KEY 1：惊讶眨眼 | 40 ms |
| `cuteblnk.anim.z` | KEY 1：可爱眨眼 | 40 ms |
| `hapyblnk.anim.z` | KEY 1：开心眨眼 | 40 ms |
| `cute.anim.z` | 电源键：可爱 | 90 ms |
| `happy.anim.z` | 电源键：开心 | 90 ms |
| `sad.anim.z` | 电源键：难过 | 90 ms |

动画采用逐帧差分和 zlib 压缩。眨眼动画需要使用 40 ms 的时间轴重采样，以保留原始 GIF 的停留时间；裁剪动画使用每 3 帧取 1 帧，以控制在 1 MB 分区容量内。重新制作资源后，应运行分区镜像校验脚本。

## 唤醒词与分区表

本板使用 `partitions/v2/4m_model.csv`（4 MB）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| `model` | 0x10000 | 384 KB | 构建期打包并烧录的唤醒词模型 `srmodels.bin` |
| `factory` | 0x70000 | 2.5 MB | 应用固件（当前镜像约 2.4 MB，余量约 250 KB） |
| `assets` | 0x300000 | 1 MB | 眼珠动画分区镜像（只读，板子专用） |

唤醒词模型由 esp-sr 在构建时按 `sdkconfig.defaults.esp32s3` 中的选择（当前为 `wn9_nihaoxiaozhi_tts`，唤醒词「你好小智」）打包成 `srmodels.bin`，并由 `idf.py flash` 烧入 `model` 分区；运行时 `esp_srmodel_init("model")` 以 mmap 方式加载。若分区表中缺少 `model` 分区，构建不会报错，但设备会打印 `MODEL_LOADER: Can not find model in partition table` 且 AFE 以 `detector: none` 运行，唤醒完全失效。

本板不启用 xiaozhi 的主题/字体 assets 系统（`CONFIG_FLASH_NONE_ASSETS=y`，无 LVGL/emote Display），`assets` 分区仅用于眼珠动画。

注意：更换分区表后必须整包 USB 重刷（`idf.py -p <端口> flash`，或烧录 `merged-binary.bin`）。本板从 `factory` 分区运行，OTA 升级会被跳过。

## 构建

在已配置 ESP-IDF 环境的终端中执行：

```sh
python3 scripts/release.py AI-dual-eyed-doll
```

资源尺寸发生变化时，可使用：

```sh
python3 scripts/dual_eye_anim/migrate_to_size.py --size 128
python3 scripts/dual_eye_anim/verify_partition_image.py
```

从 GIF 重新打包时使用 `scripts/dual_eye_anim/pack_eye_anim.py`，并将输出放入 `partition_assets/`。该目录只能保留需要写入分区的 `*.anim.z` 文件。构建系统会自动生成资源分区，通常不需要修改 `scripts/release.py`。

## 开发注意事项

- 不要修改已有板卡的引脚来适配本硬件；本板卡必须保持独立身份。
- 修改屏幕初始化时，同时检查 `config.h` 中的偏移量、颜色反转和 SPI 频率。
- 屏幕和动画依赖真实硬件验证；构建成功不能替代显示方向、颜色、裁剪和音频播放测试。
- 4 MB Flash 不适合双 OTA 分区，应用回滚和双 OTA 不应按常规板卡配置启用。
- `model` 分区（唤醒词模型）与 `assets` 分区（眼珠动画）都不要挪作他用；改动分区表属于破坏性变更，必须整包重刷。
