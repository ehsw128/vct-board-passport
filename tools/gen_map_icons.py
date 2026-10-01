#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成地图小图标 main/app_map_icons.c / app_map_icons.h。

图标源: valorant-api.com 的 maps 接口 listViewIcon(五边形盾形,透明底)。
仅收录赛事接口中出现的地图(tools/map_icons.json 的英文名字段)。
流程: 下载 PNG -> LANCZOS 缩放 20px -> RGB565A8 -> C 数组 + 查找表
运行: python tools/gen_map_icons.py   (需要 Pillow)
"""
import json
import sys
import urllib.request
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("need Pillow: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "tools" / "map_icons.json"
OUT_C = ROOT / "main" / "app_map_icons.c"
OUT_H = ROOT / "main" / "app_map_icons.h"
ICON = 20


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def sanitize(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name).lower()


def to_rgb565a8(im: Image.Image) -> bytes:
    im = im.convert("RGBA")
    im = im.resize((ICON, ICON), Image.LANCZOS)
    px = im.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(ICON):
        for x in range(ICON):
            r, g, b, a = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            rgb += v.to_bytes(2, "little")
            alpha.append(a)
    return bytes(rgb + alpha)


def c_array(name: str, data: bytes, per_line: int = 12) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ",")
    return "static const uint8_t %s[] = {\n%s\n};" % (name, "\n".join(lines).rstrip(","))


def main() -> None:
    cfg = json.loads(LIST.read_text(encoding="utf-8"))
    wanted = cfg["maps"]                       # ["Ascent", "Summit", ...]

    va = json.loads(fetch("https://valorant-api.com/v1/maps").decode("utf-8"))
    url_by_name = {}
    for m in va["data"]:
        url_by_name.setdefault(m["displayName"], m.get("listViewIcon"))

    c_bmps, c_dscs, rows = [], [], []
    for en in wanted:
        url = url_by_name.get(en)
        if not url:
            sys.exit("map not found in valorant-api: %s" % en)
        data = fetch(url)
        im = Image.open(__import__("io").BytesIO(data))
        blob = to_rgb565a8(im)
        sym = sanitize(en)
        bname, dname = "bmp_" + sym, "dsc_" + sym
        c_bmps.append(c_array(bname, blob))
        c_dscs.append(
            "static const lv_image_dsc_t %s = {\n"
            "    .header = { .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d },\n"
            "    .data_size = sizeof(%s),\n"
            "    .data = %s,\n};" % (dname, ICON, ICON, bname, bname))
        rows.append('    { "%s", &%s },' % (en, dname))
        print("  %-8s %5d B" % (en, len(blob)))

    c_src = (
        "// 自动生成 —— tools/gen_map_icons.py，勿手改。\n"
        "// 地图图标来自 valorant-api.com listViewIcon，版权归 Riot Games，仅限个人设备展示。\n"
        '#include "app_map_icons.h"\n'
        '#include "lvgl.h"\n'
        "#include <string.h>\n\n"
        + "\n\n".join(c_bmps)
        + "\n\n" + "\n\n".join(c_dscs)
        + "\n\nstatic const struct { const char *en; const void *dsc; }\n"
        "APP_MAP_ICONS[] = {\n" + "\n".join(rows) + "\n};\n\n"
        "int app_map_icons_find(const char *en) {\n"
        "    if (en == NULL || en[0] == '\\0') return -1;\n"
        "    for (unsigned i = 0; i < sizeof(APP_MAP_ICONS) / sizeof(APP_MAP_ICONS[0]); i++) {\n"
        "        if (strcmp(APP_MAP_ICONS[i].en, en) == 0) return (int)i;\n"
        "    }\n"
        "    return -1;\n"
        "}\n\n"
        "int app_map_icons_count(void) {\n"
        "    return (int)(sizeof(APP_MAP_ICONS) / sizeof(APP_MAP_ICONS[0]));\n"
        "}\n\n"
        "const void *app_map_icons_dsc(int idx) {\n"
        "    return (idx >= 0 && idx < app_map_icons_count()) ? APP_MAP_ICONS[idx].dsc : NULL;\n"
        "}\n"
    )
    OUT_C.write_text(c_src, encoding="utf-8")

    h_src = (
        "// 自动生成随 app_map_icons.c 一同维护 —— 接口由 tools/gen_map_icons.py 固定。\n"
        "#pragma once\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n\n"
        "/* 按英文地图名(mapNameEn)查图标下标；未收录返回 -1。 */\n"
        "int app_map_icons_find(const char *en);\n"
        "int app_map_icons_count(void);\n"
        "/* 返回 const lv_image_dsc_t*；越界返回 NULL。 */\n"
        "const void *app_map_icons_dsc(int idx);\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
    )
    OUT_H.write_text(h_src, encoding="utf-8")
    print("written: %s, %s" % (OUT_C.name, OUT_H.name))


if __name__ == "__main__":
    main()
