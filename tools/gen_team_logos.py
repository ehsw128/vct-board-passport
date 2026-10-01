#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成内置队伍 logo 资源 main/app_logos.c / app_logos.h。

输入: tools/team_logos.json (teamSpName -> teamDarkLogo URL)
流程: 下载 PNG -> LANCZOS 缩放 24px/32px -> RGB565A8 -> C 数组 + lv_image_dsc_t 查找表
运行: python tools/gen_team_logos.py   (需要 Pillow)
"""
import json
import struct
import sys
import urllib.request
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("need Pillow: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "tools" / "team_logos.json"
OUT_C = ROOT / "main" / "app_logos.c"
OUT_H = ROOT / "main" / "app_logos.h"
SIZES = (24, 32)


def sanitize(sp: str) -> str:
    out = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in sp)
    if not out or out[0].isdigit():
        out = "t" + out
    return out.lower()


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def png_size(data: bytes):
    return struct.unpack(">II", data[16:24])


def to_rgb565a8(im: Image.Image, size: int) -> bytes:
    """等比缩放到正方形画布居中（非正方形 logo 两侧/上下留透明边距）。"""
    im = im.convert("RGBA")
    scale = min(size / im.width, size / im.height)
    nw, nh = max(1, round(im.width * scale)), max(1, round(im.height * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(im, ((size - nw) // 2, (size - nh) // 2), im)
    im = canvas
    px = im.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            rgb += v.to_bytes(2, "little")
            alpha.append(a)
    return bytes(rgb + alpha)


def c_array(name: str, data: bytes, per_line: int = 12) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ",")
    body = "\n".join(lines).rstrip(",")
    return "static const uint8_t %s[] = {\n%s\n};" % (name, body)


def main() -> None:
    teams = json.loads(LIST.read_text(encoding="utf-8"))
    teams = {k: v for k, v in teams.items() if not k.startswith("_")}
    names = sorted(teams)
    print("teams: %d" % len(names))

    c_bmps, c_dscs, table_rows = [], [], []
    for sp in names:
        sym = sanitize(sp)
        data = fetch(teams[sp])
        im = Image.open(__import__("io").BytesIO(data))
        if im.mode != "RGBA":
            im = im.convert("RGBA")
        print("  %-6s %dx%d %5d B" % (sp, im.width, im.height, len(data)))
        for size in SIZES:
            blob = to_rgb565a8(im, size)
            bname = "bmp_%s_%d" % (sym, size)
            dname = "dsc_%s_%d" % (sym, size)
            c_bmps.append(c_array(bname, blob))
            c_dscs.append(
                "static const lv_image_dsc_t %s = {\n"
                "    .header = { .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d },\n"
                "    .data_size = sizeof(%s),\n"
                "    .data = %s,\n};"
                % (dname, size, size, bname, bname))
        table_rows.append('    { "%s", &dsc_%s_24, &dsc_%s_32 },' % (sp, sym, sym))

    c_src = (
        "// 自动生成 —— tools/gen_team_logos.py，勿手改。\n"
        "// 队伍 logo 来自 vct.qq.com 赛事接口 teamDarkLogo，深色背景使用。\n"
        "// 版权提示: 队伍图形为 Riot Games 及各俱乐部商标，仅限个人设备展示用途。\n"
        '#include "app_logos.h"\n'
        '#include "lvgl.h"\n'
        "#include <string.h>\n\n"
        + "\n\n".join(c_bmps)
        + "\n\n" + "\n\n".join(c_dscs)
        + "\n\nstatic const struct {\n"
        "    const char *sp;\n"
        "    const void *d24;\n"
        "    const void *d32;\n"
        "} APP_LOGOS[] = {\n" + "\n".join(table_rows) + "\n};\n\n"
        "int app_logos_find(const char *sp) {\n"
        "    if (sp == NULL || sp[0] == '\\0') return -1;\n"
        "    for (unsigned i = 0; i < sizeof(APP_LOGOS) / sizeof(APP_LOGOS[0]); i++) {\n"
        "        if (strcmp(APP_LOGOS[i].sp, sp) == 0) return (int)i;\n"
        "    }\n"
        "    return -1;\n"
        "}\n\n"
        "int app_logos_count(void) {\n"
        "    return (int)(sizeof(APP_LOGOS) / sizeof(APP_LOGOS[0]));\n"
        "}\n\n"
        "const void *app_logos_dsc_24(int idx) {\n"
        "    return (idx >= 0 && idx < app_logos_count()) ? APP_LOGOS[idx].d24 : NULL;\n"
        "}\n\n"
        "const void *app_logos_dsc_32(int idx) {\n"
        "    return (idx >= 0 && idx < app_logos_count()) ? APP_LOGOS[idx].d32 : NULL;\n"
        "}\n"
    )
    OUT_C.write_text(c_src, encoding="utf-8")

    h_src = (
        "// 自动生成随 app_logos.c 一同维护 —— 接口由 tools/gen_team_logos.py 固定。\n"
        "#pragma once\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n\n"
        "/* 按 teamSpName 查内置 logo 下标；未收录返回 -1。 */\n"
        "int app_logos_find(const char *sp);\n"
        "int app_logos_count(void);\n"
        "/* 返回 const lv_image_dsc_t*，调用方转型；越界返回 NULL。 */\n"
        "const void *app_logos_dsc_24(int idx);\n"
        "const void *app_logos_dsc_32(int idx);\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
    )
    OUT_H.write_text(h_src, encoding="utf-8")
    print("written: %s (%.1f KB), %s" % (OUT_C.name, OUT_C.stat().st_size / 1024, OUT_H.name))


if __name__ == "__main__":
    main()
