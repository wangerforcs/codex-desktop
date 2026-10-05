# Codex Desktop · 布拉格

面向 Waveshare ESP32-S3-Touch-LCD-4.3C 的独立 Arduino 项目。屏幕显示布拉格主题画面、Codex 套餐用量的短/长窗口剩余百分比及重置日期时间，周额度显示为“百分比 + 7D”。点击背景切换日景和傍晚；点击鸽子会让它原地跳一下再落回原处；点击额度文字区域可立即请求刷新。项目不需要原厂固件，也不会读取或上传原厂固件备份。

## 目录

- `codex_desktop.ino`：Arduino 草图与 LVGL 界面。
- `quota_bridge.py`：电脑端桥接服务，通过本机已登录的 Codex app-server 读取额度。
- `quota_config.example.h`：网络配置模板；复制为 `quota_config.h` 后填写个人 Wi-Fi 信息。
- `src/`：LCD、触摸驱动和已编码的布拉格图片。
- `assets/`：生成图片的源 PNG、预览图和可选的资源转换脚本。
- `config/lv_conf.h`：本项目使用的 LVGL 8.4.0 配置。

`pictures/` 中的个人参考照片默认被 Git 忽略，编译不需要它们；如打算公开原照片，请自行确认隐私与发布权利。

## Arduino 准备

初次配置请先看 [Waveshare ESP32-S3-Touch-LCD-4.3C Arduino 官方教程](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4.3C/Arduino/)，其中有开发板包、LVGL 8.4.0 和 `lv_conf.h` 的安装说明。本项目的板型和分区选项还需按下面设置。

1. 安装 Espressif ESP32 Arduino 开发板包（本项目用 3.1.1 验证）及 LVGL **8.4.0**。LVGL 9.x 的接口不兼容。
2. 将 `config/lv_conf.h` 复制到 Arduino Sketchbook 的 `libraries` 目录，与 `lvgl` 文件夹并列，使布局为 `libraries/lv_conf.h` 与 `libraries/lvgl/`。不要覆盖其他项目专用配置而不备份。
3. 将 `quota_config.example.h` 复制为 `quota_config.h`，填入 2.4 GHz Wi-Fi 的 SSID 和密码。`QUOTA_PC_IP` 默认留空，由板子自动发现电脑；不需要写 DHCP 地址。这个本地配置文件不会被 Git 提交。
4. 在 Arduino IDE 打开 `codex_desktop.ino`，手动选择开发板 **`ESP32S3 Dev Module`**、Flash Size **16MB**、PSRAM **OPI PSRAM**、USB CDC On Boot **Enabled**、Partition Scheme **`16M Flash (3MB APP/9.9MB FATFS)`**。编译可能需要数分钟。

上传会替换设备当前固件；请先自行备份需要保留的内容，minimax原始内容在original_flash.bin。

## 电脑端运行

电脑需已安装并登录 Codex CLI，且与屏幕位于允许设备互访的同一局域网。电脑可以连接 5 GHz；ESP32-S3 必须连接 2.4 GHz。

在本项目目录运行：

```powershell
python .\quota_bridge.py --once
python .\quota_bridge.py
```

第一行只检查额度读取，第二行启动常驻服务。保持窗口和电脑运行；修改桥接脚本后需重启服务。Windows 防火墙只允许**专用网络**：TCP 8787 提供只读 `/quota`，UDP 8788 用于自动发现。不要向公网转发这两个端口。网络隔离若阻止广播，可在路由器上为电脑保留 DHCP 地址，再填入 `QUOTA_PC_IP` 作为备用。

电脑端每 120 秒刷新一次 Codex 数据，板子每 30 秒拉取一次。短、长窗口分别显示 `100 − usedPercent`；重置时间由电脑按其本地时区换算为 `月/日 时:分`。屏幕如果失去更新，会标记 `STALE / LAN`，不会把旧读数伪装成实时数据。桥接服务只向板子发送百分比、窗口时长和重置时间，不发送 Codex 令牌、API Key 或账号 ID。这里显示的是 Codex/ChatGPT 套餐用量窗口，不是 OpenAI API 的 RPM/TPM 限速。

## 图片与来源

布拉格日景及傍晚图片是参考项目作者拍摄的查理大桥与鸽子照片生成的艺术画面，不是原照片直接裁切。当前固件使用新增的 `prague_day_clear_source.png`、`prague_dusk_clear_source.png` 无鸽子背景，以及 `pigeon_stand_source.png`、`pigeon_fly_source.png` 透明鸽子素材；原始带鸽子版本仍保留在 `assets/`。标题现由 LVGL 字体绘制为 `PRAGUE`，旧的透明中文标题 PNG 也保留作历史素材。资源已经以 RGB565 编入 `src/art/prague_images.cpp`，运行时不需要 SD 卡。若修改源 PNG，可在项目目录运行 `python assets/convert_for_lvgl.py`（需要 Pillow）重新生成图片代码与预览图；转换脚本不依赖 Windows 字体或机器绝对路径。

LCD、触摸相关代码基于 Waveshare 官方 Arduino 示例及 Espressif 驱动，原有版权标识保留；相关 Apache-2.0 文本见 `THIRD_PARTY_APACHE_2_0.txt`。本目录没有替你选定整个项目及生成图片的公开许可，发布到 GitHub 前请自行决定许可证与图片使用条款。
