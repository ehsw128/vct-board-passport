<p align="right"><strong>English</strong> · <a href="README.zh_CN.md">简体中文</a></p>

# Fonts

The VCT board includes LVGL 9 Noto Sans SC subsets at 16 and 22 pixels in `noto_sc_16.c` and `noto_sc_22.c`. Both use 4 bits per pixel without compression and are compiled into the firmware by `main/CMakeLists.txt`.

The character set in `tools/font/chars.txt` covers printable ASCII, GB2312 level-one Chinese characters and punctuation, plus characters from the sample match data and UI. Rare characters in future live data may still lack glyphs. The two fonts use about 1.5 MB of Flash; bitmap data stays in read-only storage.

The source font is [Noto Sans SC](https://github.com/google/fonts/tree/main/ofl/notosanssc), licensed under the [SIL Open Font License 1.1](https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt). Generated font sources retain that license. To regenerate, obtain `NotoSansSC[wght].ttf`, install `lv_font_conv` 1.5.3, and use `tools/font/build_font.js` with the desired pixel size and output path. That helper currently contains paths from the original development machine and must be adapted before reuse.
