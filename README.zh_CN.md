[English](README.md) · **简体中文**

# AI Passport 无畏契约比赛看板

这是运行在 FoloToy AI Passport 上的比赛赛程与结果看板。联网固件通过 Wi-Fi 获取近期无畏契约赛事；离线预览固件内置示例比赛，方便检查屏幕布局和按键。

## 快速运行

1. 将 [V18 联网固件](firmware/VCT-Board-v18-full.bin) 从地址 `0x0` 刷入 AI Passport（ESP32-C3，8 MB Flash）。
2. 用手机连接设备热点 `VCTBoard-XXXX`，打开 `http://192.168.4.1`，选择 **2.4 GHz** Wi-Fi 并输入密码。
3. 等待设备联网并同步赛程。如果一次同步失败，设备会自动重试。

## 按键操作

| 界面 | 上键 / 下键 | OK 键 |
| --- | --- | --- |
| 日期 | 选择更早 / 更晚的日期 | 查看当天比赛 |
| 比赛列表 | 选择比赛；越过列表边界时切换日期 | 进入比赛详情 |
| 比赛详情 | 滚动详情 | 返回比赛列表 |

在日期或比赛列表界面长按 **OK**，可重新进入 Wi-Fi 配网。

## 离线界面预览

如需只检查界面，可将 [V18 离线预览固件](firmware/VCT-Board-v18-UI-preview-full.bin) 从 `0x0` 刷入设备。它使用内置示例比赛，无需联网。两份固件任选一份刷入；预览版不会获取实时赛果。

本仓库包含完整源码。重新编译请使用 ESP-IDF 5.5.3，目标芯片为 `esp32c3`；详见[构建说明](docs/development/engineering/build-and-test.zh_CN.md)。硬件和基础软件来自 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport)。
