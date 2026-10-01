# 生成 LVGL 中文字体子集的符号表（UTF-8 文本，每行字符集连续拼接）
# 覆盖：ASCII 可打印区 + GB2312 一级常用汉字 3755 + GB2312 标点符号区
#       + 样例 API JSON 中出现的全部非 ASCII 字符（含地图名/赛区名等二级字）+ UI 词
import json

chars = set()
# ASCII 0x20-0x7E
for c in range(0x20, 0x7F):
    chars.add(chr(c))

# GB2312 一级汉字：区位 16-55 区（0xB0-0xD7），每区 1-94 位（0xA1-0xFE）
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            pass

# GB2312 符号区第 1 区（全角标点）：0xA1A1-0xA1FE
for lo in range(0xA1, 0xFF):
    try:
        chars.add(bytes([0xA1, lo]).decode('gb2312'))
    except UnicodeDecodeError:
        pass

# 样例 API 数据中的全部非 ASCII 字符（地图名、赛区名、赛事名等）
samples = ['val_match_1000074.json', 'stat_match.json', 'VAL_Match_day1.json',
           'VAL_Match_day2.json', 'VAL_Match_day3.json', 'VAL_FGameList.json',
           'sch_match.json', 'VAL_SGameList_display.json']
for f in samples:
    try:
        text = open(r'D:\python\pj' + '\\' + f, encoding='utf-8').read()
        for ch in text:
            if ord(ch) > 0x7E:
                chars.add(ch)
    except OSError as e:
        print('skip', f, e)

# 应用 UI 词表
ui = ('赛程看板进行中即将开始已结束连接网络配网热点名称密码保存清除重试离线刷新'
      '返回详情胜利比分地图顺序轮空胜者组败者组决赛半四分之一淘汰揭幕战常规赛'
      '今天明天昨天周一二三四五六时分秒等待中拉取失败点击上下浏览确定长按短按'
      '未连接已断开扫码浏览器手机输入验证成功失败状态退出加载错误暂无数据比赛'
      '队主场客场总耗时回合进攻防守换边加时赛赛点总局数第一二三四五季排名积分')
chars.update(ui)

out = ''.join(sorted(chars))
with open(r'D:\python\pj\chars.txt', 'w', encoding='utf-8') as fp:
    fp.write(out)
print('charset size:', len(out))
