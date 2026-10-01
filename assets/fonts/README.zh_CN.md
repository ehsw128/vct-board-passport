<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 字库（fonts）

本目录存放赛事看板应用使用的 LVGL 字库源码。

## noto_sc_16.c / noto_sc_22.c

无畏契约赛事看板（VCT Board）中文 UI 字库，LVGL `lv_font_t` 源码，由 `lv_font_conv` 从 Noto Sans SC 生成。

| 属性 | 值 |
| --- | --- |
| 字族 | Noto Sans SC（思源黑体谷歌分支，可变字重版） |
| 来源 | google/fonts 仓库 `ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf`（jsdelivr CDN 下载） |
| 许可 | SIL Open Font License 1.1（允许嵌入与再分发，OFL 保留声明见下） |
| 字号 | 16 px（列表副行/标签）、22 px（队名/标题） |
| 像素格式 | 4 bpp，抗锯齿，`--no-compress`（LVGL 9 渲染兼容优先） |
| 格式 | `--format lvgl`，结构体名 `noto_sc_16` / `noto_sc_22` |
| 字符集 | 3956 字符：ASCII 可打印区（0x20–0x7E）、GB2312 一级常用字 3755 字（0xB0A1–0xD7F9）、GB2312 第一区标点符号（0xA1A1–0xA1FE）、赛事样例数据与 UI 词中的补充字符（含 U+00B7 间隔号） |
| 放置路径 | `assets/fonts/noto_sc_16.c`、`assets/fonts/noto_sc_22.c`，由 `main/CMakeLists.txt` 直接编入固件 |

### Flash 与 RAM 影响

- 16 px 源码约 3.2 MB，编译后约 550 KB Flash；22 px 源码约 5.3 MB，编译后约 980 KB Flash。
- LVGL 以 `.rodata` 引用位图数据，运行时无额外堆开销；ESP32-C3 无 PSRAM，勿再扩大字符集。

### 转换命令

工具：Node.js 20 + `lv_font_conv`（npm，1.5.x）。符号表为 UTF-8 文本文件（每行/连续字符皆可，空白忽略）。

```sh
lv_font_conv \
  --font NotoSansSC.ttf \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --symbols "$(cat chars.txt)" \
  -o noto_sc_16.c
```

22 px 同理替换 `--size` 与输出文件。注意：PowerShell 对超长命令行参数有长度限制，建议用 Node 包装脚本以 `execFileSync` 数组传参（见仓库构建记录）。

### 许可声明

Noto Sans SC 以 SIL OFL 1.1 分发，本仓库的字库是其派生字体，随源码一同分发时保留本声明即可；OFL 授权文本见上游 <https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt>。
