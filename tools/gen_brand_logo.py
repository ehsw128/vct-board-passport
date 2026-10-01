#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成无畏契约品牌标志 main/app_brand.c / app_brand.h。

输入: tools/brand/V_Logomark_Red.png (Riot 官方 VALORANT Asset Kit,
      https://playvalorant.com/en-gb/news/game-updates/valorant-asset-kit/)
流程: 裁透明留白 -> LANCZOS 缩放到目标高度(保持比例) -> RGB565A8 -> C 数组
运行: python tools/gen_brand_logo.py   (需要 Pillow)
"""
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("need Pillow: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "brand" / "V_Logomark_Red.png"
OUT_C = ROOT / "main" / "app_brand.c"
OUT_H = ROOT / "main" / "app_brand.h"
TARGET_H = 20          # 详情页顶栏:与 16px 文字行对齐


def to_rgb565a8(im: Image.Image):
    im = im.convert("RGBA")
    im = im.crop(im.getbbox())                       # 去透明留白
    w = max(1, round(im.width * TARGET_H / im.height))
    im = im.resize((w, TARGET_H), Image.LANCZOS)
    px = im.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(TARGET_H):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            rgb += v.to_bytes(2, "little")
            alpha.append(a)
    return bytes(rgb + alpha), w


def c_array(name: str, data: bytes, per_line: int = 12) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ",")
    return "static const uint8_t %s[] = {\n%s\n};" % (name, "\n".join(lines).rstrip(","))


def main() -> None:
    if not SRC.exists():
        sys.exit("missing source: %s" % SRC)
    blob, w = to_rgb565a8(Image.open(SRC))
    print("brand logo: %dx%d, %d bytes" % (w, TARGET_H, len(blob)))

    c_src = (
        "// 自动生成 —— tools/gen_brand_logo.py，勿手改。\n"
        "// VALORANT 红色 V 形标志来自 Riot 官方 Asset Kit，仅限个人设备展示用途。\n"
        '#include "app_brand.h"\n'
        '#include "lvgl.h"\n\n'
        + c_array("brand_data", blob)
        + "\n\nstatic const lv_image_dsc_t brand_dsc = {\n"
        "    .header = { .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d },\n"
        "    .data_size = sizeof(brand_data),\n"
        "    .data = brand_data,\n};\n\n"
        "const void *app_brand_dsc(void) {\n"
        "    return &brand_dsc;\n"
        "}\n" % (w, TARGET_H)
    )
    OUT_C.write_text(c_src, encoding="utf-8")

    h_src = (
        "// 自动生成随 app_brand.c 一同维护 —— 接口由 tools/gen_brand_logo.py 固定。\n"
        "#pragma once\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n\n"
        "/* 返回 const lv_image_dsc_t*（VALORANT 红色 V 标志）。 */\n"
        "const void *app_brand_dsc(void);\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
    )
    OUT_H.write_text(h_src, encoding="utf-8")
    print("written: %s, %s" % (OUT_C.name, OUT_H.name))


if __name__ == "__main__":
    main()
