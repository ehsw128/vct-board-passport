// build_font.js —— 从 chars.txt 读符号表，调用 lv_font_conv 生成 LVGL 字体源码
// 用法: node build_font.js <size> <out.c>
const { execFileSync } = require('child_process');
const path = require('path');

const NODE_MODULES = 'D:/python/pj/node/node-v20.19.0-win-x64/node_modules';
const size = process.argv[2];
const out = process.argv[3];
if (!size || !out) {
  console.error('usage: node build_font.js <size> <out.c>');
  process.exit(1);
}

const syms = require('fs').readFileSync('D:/python/pj/chars.txt', 'utf8');
const args = [
  path.join(NODE_MODULES, 'lv_font_conv/lv_font_conv.js'),
  '--font', 'D:/python/pj/NotoSansSC.ttf',
  '--size', String(size),
  '--bpp', '4',
  '--format', 'lvgl',
  '--no-compress',
  '--symbols', syms,
  '-o', out,
];
execFileSync(process.execPath, args, { stdio: 'inherit' });
console.log('done:', out);
