// main/app_ui.c —— 赛事看板界面。
//
// 240×320 竖屏布局:
//   ┌──────────────────────┐
//   │ VCT 赛程     20:45 87%│  顶栏 30px
//   ├──────────────────────┤
//   │ ● 进行中              │  分组标题 22px
//   │ ┌──────────────────┐ │
//   │ │ 100T   1:0   T1  │ │  条目卡 52px(22px 队名+比分)
//   │ │ 进行中 · BO3     │ │           (16px 副行)
//   │ └──────────────────┘ │
//   │  ...                  │  body 264px 可滚动
//   ├──────────────────────┤
//   │ 上下浏览·OK查看·长按配网│  提示 26px
//   └──────────────────────┘
//
// 设计约定:队徽为编译期内置(见 tools/gen_team_logos.py,24/32px RGB565A8),
// 不做运行时远程下载(无 PSRAM,不能解析网络 PNG);未收录队伍回退为纯文字。
// 界面全部使用 Noto Sans SC 中文子集字体(见 assets/fonts/)。
#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_model.h"
#include "app_state.h"
#include "app_data.h"
#include "app_logos.h"
#include "app_brand.h"
#include "app_map_icons.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#ifdef APP_OFFLINE_PREVIEW
#include "app_preview.h"
#endif

// 屏幕可见的固件版本标记:用于确认实际刷入的是哪一版。
#ifdef APP_OFFLINE_PREVIEW
#define APP_FW_TAG   "FW v18 UI"
#else
#define APP_FW_TAG   "FW v18 0929"
#endif

#define TAG "app_ui"

// ---- 主题 ----
#define C_BG        0x0D1117
#define C_PANEL     0x161B22
#define C_PANEL_SEL 0x1B2A3D
#define C_INK       0xE6EDF3
#define C_SUB       0x8B949E
#define C_LIVE      0xF85149
#define C_UPCOMING  0x58A6FF
#define C_DONE      0x8B949E
#define C_ACCENT    0x1F6FEB
#define C_WIN       0x3FB950
#define C_GOLD      0xF5B342   // 参考图:胜方比分/下划线
#define C_LOGO_BG   0x202936   // 队徽深色圆角底板
#define C_DIVIDER   0x30363D   // 分隔线

LV_FONT_DECLARE(noto_sc_16);
LV_FONT_DECLARE(noto_sc_22);

typedef enum {
    PAGE_BOOT = 0,
    PAGE_DATES,          // 一级:日期选择
    PAGE_DAY,            // 二级:当日比赛
    PAGE_DETAIL,         // 三级:逐图详情
    PAGE_PROV,
} page_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *topbar;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_clock;
    lv_obj_t *lbl_batt;
    lv_obj_t *body;          // 每次切页 clean 重建
    lv_obj_t *lbl_hint;

    page_t page;
    app_dates_view_t dates; // 一级:日期聚合
    app_day_view_t day;     // 二级:当日比赛
    app_match_t raw[APP_RAW_MAX];
    int raw_count;

    int64_t showing_detail;  // 详情页当前 match id;0=无
    bool detail_ready;       // 详情数据已渲染
    int last_batt;           // -1 未知
    int last_clock_min;      // 已绘制的 HH*60+M;-1 强制刷新
    int64_t data_version;    // 上次列表数据版本(last_ok_ms)
    app_data_status_t data_status_seen;
    bool time_ready_seen;
} ui_state_t;

static ui_state_t s_ui;

static int64_t ui_now(void) {
#ifdef APP_OFFLINE_PREVIEW
    return app_preview_now();
#else
    return time(NULL);
#endif
}

// ================================================================ 工具

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            lv_align_t align, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (align != LV_ALIGN_DEFAULT) {
        lv_obj_align(l, align, x, y);
    }
    return l;
}

static void style_card(lv_obj_t *card, uint32_t bg) {
    lv_obj_set_style_bg_color(card, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
}

// 电量图标化:文本 "87%" 或 "充电/未知" 不显示
static void refresh_batt(void) {
    int soc = bsp_battery_soc();
    s_ui.last_batt = soc;
    if (soc < 0) {
        lv_label_set_text(s_ui.lbl_batt, "");
        return;
    }
    lv_label_set_text_fmt(s_ui.lbl_batt, "%d%%", soc);
}

static void refresh_clock(bool force) {
    if (app_data_is_preview()) {
        lv_label_set_text(s_ui.lbl_clock, "19:00");
        return;
    }
    if (!app_wifi_sntp_synced()) {
        if (s_ui.last_clock_min != -1 || force) {
            lv_label_set_text(s_ui.lbl_clock, "--:--");
            s_ui.last_clock_min = -1;
        }
        return;
    }
    time_t now_t = time(NULL);
    struct tm tm_now;
    // 固定东八区分解,与 app_state.c 的显示口径一致
    time_t local = now_t + 8 * 3600;
    gmtime_r(&local, &tm_now);
    int minutes = tm_now.tm_hour * 60 + tm_now.tm_min;
    if (minutes != s_ui.last_clock_min || force) {
        lv_label_set_text_fmt(s_ui.lbl_clock, "%02d:%02d",
                              tm_now.tm_hour, tm_now.tm_min);
        s_ui.last_clock_min = minutes;
    }
}

// ================================================================ 列表页

static void add_section_header(lv_obj_t *parent, const char *text,
                               uint32_t color, int y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 224, 22);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = make_label(row, &noto_sc_16, color, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(l, text);
}

// ================================================================ 日期页(一级)

static const char *const WD_NAMES[] =
    { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

// 从日期键 "YYYY-MM-DD" 解析月/日。
static void parse_date_key(const char *key, int *mo, int *d) {
    int y;
    sscanf(key, "%d-%d-%d", &y, mo, d);
}

// 右侧状态药丸:足够容纳四个汉字。
static void make_status_pill(lv_obj_t *parent, const char *text,
                             uint32_t bg, uint32_t fg) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 72, 22);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_t *l = lv_label_create(pill);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_set_style_text_font(l, &noto_sc_16, 0);
    lv_obj_center(l);
    lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -4, 0);
}

// 单个日期卡(50px)。
static void add_date_card(const app_date_entry_t *e, bool selected, int y) {
    lv_obj_t *card = lv_obj_create(s_ui.body);
    lv_obj_set_size(card, 224, 50);
    lv_obj_set_pos(card, 0, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    style_card(card, selected ? C_PANEL_SEL : C_PANEL);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(card, selected ? 1 : 0, 0);
    if (selected) {
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_set_size(bar, 3, 38);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_UPCOMING), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, -6, 0);
    }

    int mo, d;
    parse_date_key(e->key, &mo, &d);
    lv_obj_t *l1 = make_label(card, &noto_sc_16, C_INK,
                              LV_ALIGN_TOP_LEFT, 12, 5);
    lv_label_set_text_fmt(l1, "%d月%d日", mo, d);
    lv_obj_t *l2 = make_label(card, &noto_sc_16, C_SUB,
                              LV_ALIGN_BOTTOM_LEFT, 12, -5);
    lv_label_set_text_fmt(l2, "%s · 共%d场", WD_NAMES[e->wday],
                          e->match_count);

    // 状态药丸
    if (e->group == APP_GROUP_PAST) {
        make_status_pill(card, "已结束", 0x21262D, C_SUB);
    } else if (e->group == APP_GROUP_FUTURE) {
        make_status_pill(card, "即将开始", 0x12233B, C_UPCOMING);
    } else if (e->has_live) {
        make_status_pill(card, "进行中", 0x3B1418, C_LIVE);
    } else {
        make_status_pill(card, "今天", 0x12233B, C_UPCOMING);
    }
}

static void render_dates(void) {
    lv_label_set_text(s_ui.lbl_hint, "上下选日期 · OK 进入 · 长按配网");
    lv_obj_clean(s_ui.body);

    lv_obj_t *heading = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                   LV_ALIGN_TOP_LEFT, 4, 0);
    if (app_data_is_preview()) {
        lv_label_set_text(heading, "离线演示");
    } else {
        lv_label_set_text_fmt(heading, "赛程日期 · %d场", s_ui.raw_count);
    }
    lv_obj_t *tag = make_label(s_ui.body, &noto_sc_16, C_SUB,
                               LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_label_set_text(tag, "v18");

    if (s_ui.dates.count == 0) {
        lv_obj_t *empty = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                     LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_set_width(empty, 220);
        app_data_status_t status = app_data_status();
        if (status == APP_DATA_ERR) {
            if (app_data_diag_err() == -4) {
                lv_label_set_text(empty, "网络已连接，校时未完成\n请检查网络或稍后重试");
            } else if (app_data_diag_err() == 28674) {
                lv_label_set_text_fmt(empty,
                    "建连失败 28674 · 自动重试\nTLS:%d M:%d\n证书:%X 网络:%d",
                    app_data_diag_tls_err(), app_data_diag_tls_code(),
                    app_data_diag_tls_flags(), app_data_diag_errno());
            } else {
                lv_label_set_text_fmt(empty, "赛程同步失败 · 自动重试\n错误码 %d / 已收 %d 字节",
                                      app_data_diag_err(), app_data_diag_bytes());
            }
        } else if (app_wifi_is_sta_connected() && !app_wifi_can_fetch() &&
                   !app_data_is_preview()) {
            lv_label_set_text(empty, "配网成功\n正在准备赛程同步…");
        } else if (!app_wifi_sntp_synced() && !app_data_is_preview()) {
            lv_label_set_text(empty, "网络已连接\n正在校准时间…");
        } else {
            lv_label_set_text(empty, "正在同步赛程…");
        }
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    // 头部分组编码:过去=1 未来=2 今天无直播=3 今天有直播=4
    int prev_header = 0;
    int y = 28;
    lv_obj_t *sel_obj = NULL;
    for (int i = 0; i < s_ui.dates.count; i++) {
        const app_date_entry_t *e = &s_ui.dates.dates[i];
        int hc;
        const char *htext;
        uint32_t hcolor;
        if (e->group == APP_GROUP_PAST) {
            hc = 1; htext = "最近结束"; hcolor = C_DONE;
        } else if (e->group == APP_GROUP_FUTURE) {
            hc = 2; htext = "即将开始"; hcolor = C_UPCOMING;
        } else if (e->has_live) {
            hc = 4; htext = "进行中"; hcolor = C_LIVE;
        } else {
            hc = 3; htext = "今天"; hcolor = C_UPCOMING;
        }
        if (hc != prev_header) {
            add_section_header(s_ui.body, htext, hcolor, y);
            y += 24;
            prev_header = hc;
        }
        add_date_card(e, i == s_ui.dates.selected, y);
        y += 56;
        if (i == s_ui.dates.selected) {
            sel_obj = lv_obj_get_child(s_ui.body, -1);
        }
    }
    if (sel_obj) lv_obj_scroll_to_view(sel_obj, LV_ANIM_OFF);
}

// 重建一级数据;preserve=true 时尽量保持当前选中日期。
static void build_dates_data(bool preserve) {
    char keep[APP_DATE_KEY_MAX];
    bool have_keep = false;
    if (preserve && s_ui.dates.selected >= 0 &&
        s_ui.dates.selected < s_ui.dates.count) {
        snprintf(keep, sizeof(keep), "%s",
                 s_ui.dates.dates[s_ui.dates.selected].key);
        have_keep = true;
    }
    s_ui.raw_count = app_data_get_matches(s_ui.raw, APP_RAW_MAX);
    app_state_build_dates(s_ui.raw, s_ui.raw_count, ui_now(), &s_ui.dates);
    if (have_keep) {
        for (int i = 0; i < s_ui.dates.count; i++) {
            if (strcmp(s_ui.dates.dates[i].key, keep) == 0) {
                s_ui.dates.selected = i;
                break;
            }
        }
    }
}

static void build_dates_page(void) {
    s_ui.data_status_seen = app_data_status();
    s_ui.time_ready_seen = app_wifi_sntp_synced();
    int64_t v = app_data_last_ok_ms();
    if (v != s_ui.data_version) {
        s_ui.data_version = v;
        build_dates_data(true);
    }
    render_dates();
}

// ================================================================ 当日比赛页(二级)

// 26x26 小队徽底板(左/右)。
static void add_day_badge(lv_obj_t *card, const app_match_t *m, int side) {
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_set_size(box, 26, 26);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box, 7, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_LOGO_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_align(box, side == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 side == 0 ? 8 : -8, 8);

    if (m->logo[side] >= 0) {
        const lv_image_dsc_t *dsc = app_logos_dsc_24(m->logo[side]);
        if (dsc) {
            lv_obj_t *img = lv_image_create(box);
            lv_image_set_src(img, dsc);
            lv_obj_center(img);
            return;
        }
    }
    lv_obj_t *fb = lv_label_create(box);
    const char *sp = m->sp[side];
    lv_label_set_text(fb, sp[0] ? sp : "?");
    lv_obj_set_style_text_font(fb, &noto_sc_16, 0);
    lv_obj_set_style_text_color(fb, lv_color_hex(C_SUB), 0);
    lv_obj_center(fb);
}

static void add_day_card(const app_match_t *m, bool selected, int y) {
    lv_obj_t *card = lv_obj_create(s_ui.body);
    lv_obj_set_size(card, 224, 66);
    lv_obj_set_pos(card, 0, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    style_card(card, selected ? C_PANEL_SEL : C_PANEL);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(card, selected ? 1 : 0, 0);

    // 顶行:开赛时间 + BO + 状态
    struct tm tmv;
    gmtime_r(&(const time_t){ m->start_ts + 8 * 3600 }, &tmv);
    lv_obj_t *lt = make_label(card, &noto_sc_16, C_SUB,
                              LV_ALIGN_TOP_LEFT, 10, 6);
    lv_label_set_text_fmt(lt, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    if (m->bo) {
        lv_obj_t *bl = make_label(card, &noto_sc_16, C_UPCOMING,
                                  LV_ALIGN_TOP_LEFT, 58, 6);
        lv_label_set_text_fmt(bl, "BO%d", m->bo);
    }
    const char *stext;
    uint32_t scolor;
    if (m->status == APP_STATUS_LIVE) { stext = "进行中"; scolor = C_LIVE; }
    else if (m->status == APP_STATUS_DONE) { stext = "已结束"; scolor = C_SUB; }
    else { stext = "未开始"; scolor = C_UPCOMING; }
    lv_obj_t *sl = make_label(card, &noto_sc_16, scolor,
                              LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_label_set_text(sl, stext);

    // 队徽底板
    add_day_badge(card, m, 0);
    add_day_badge(card, m, 1);

    // 队名
    const char *n0 = m->sp[0][0] ? m->sp[0]
                   : m->team_name[0][0] ? m->team_name[0] : "待定";
    const char *n1 = m->sp[1][0] ? m->sp[1]
                   : m->team_name[1][0] ? m->team_name[1] : "待定";
    lv_obj_t *nm0 = make_label(card, &noto_sc_16, C_INK,
                               LV_ALIGN_LEFT_MID, 40, 8);
    lv_obj_set_width(nm0, 48);
    lv_obj_set_style_text_align(nm0, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(nm0, n0);
    lv_obj_t *nm1 = make_label(card, &noto_sc_16, C_INK,
                               LV_ALIGN_RIGHT_MID, -40, 8);
    lv_obj_set_width(nm1, 48);
    lv_obj_set_style_text_align(nm1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(nm1, n1);

    // 中央比分
    if (m->status == APP_STATUS_UPCOMING) {
        lv_obj_t *cs = make_label(card, &noto_sc_16, C_SUB,
                                  LV_ALIGN_CENTER, 0, 8);
        lv_label_set_text(cs, "- : -");
    } else {
        bool win0 = m->score[0] > m->score[1];
        bool win1 = m->score[1] > m->score[0];
        uint32_t a = win0 ? C_GOLD : C_INK;
        uint32_t b = win1 ? C_GOLD : C_INK;
        lv_obj_t *a0 = make_label(card, &noto_sc_16, a,
                                  LV_ALIGN_CENTER, -20, 8);
        lv_label_set_text_fmt(a0, "%d", m->score[0]);
        lv_obj_t *cc = make_label(card, &noto_sc_16, C_SUB,
                                  LV_ALIGN_CENTER, -2, 8);
        lv_label_set_text(cc, ":");
        lv_obj_t *b0 = make_label(card, &noto_sc_16, b,
                                  LV_ALIGN_CENTER, 16, 8);
        lv_label_set_text_fmt(b0, "%d", m->score[1]);
    }
}

static void render_day(void) {
    lv_label_set_text(s_ui.lbl_hint, "上下浏览比赛/日期 · OK详情");
    lv_obj_clean(s_ui.body);

    int mo, d;
    parse_date_key(s_ui.day.key, &mo, &d);
    // 当日头部
    lv_obj_t *hd = make_label(s_ui.body, &noto_sc_16, C_INK,
                              LV_ALIGN_TOP_LEFT, 4, 0);
    // 取该日期星期:从原始条目不方便,用 day 视图内第一场比赛重算
    int wday = 0;
    if (s_ui.day.count > 0) {
        const app_match_t *m0 = &s_ui.raw[s_ui.day.raw_idx[0]];
        char dk[APP_DATE_KEY_MAX];
        int wd;
        // 与 app_state 同口径:直接调本地日期键需要静态函数,
        // 这里用结构里无 wday,退而用 gmtime
        struct tm tm;
        gmtime_r(&(const time_t){ m0->start_ts + 8 * 3600 }, &tm);
        (void)dk; (void)wd;
        wday = (int)tm.tm_wday;
    }
    lv_label_set_text_fmt(hd, "%d月%d日 %s", mo, d, WD_NAMES[wday]);
    lv_obj_t *cnt = make_label(s_ui.body, &noto_sc_16, C_SUB,
                               LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_label_set_text_fmt(cnt, "共 %d 场", s_ui.day.count);

    if (s_ui.day.count == 0) {
        lv_obj_t *empty = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                     LV_ALIGN_TOP_MID, 0, 80);
        lv_label_set_text(empty, "当日暂无比赛");
        return;
    }

    lv_obj_t *sel_obj = NULL;
    for (int i = 0; i < s_ui.day.count; i++) {
        const app_match_t *m = &s_ui.raw[s_ui.day.raw_idx[i]];
        add_day_card(m, i == s_ui.day.selected, 28 + i * 72);
        if (i == s_ui.day.selected) {
            sel_obj = lv_obj_get_child(s_ui.body, -1);
        }
    }
    if (sel_obj) lv_obj_scroll_to_view(sel_obj, LV_ANIM_OFF);
}

// 进入当日页(从日期页 OK)。
static void enter_day(const char *key) {
    s_ui.page = PAGE_DAY;
    s_ui.raw_count = app_data_get_matches(s_ui.raw, APP_RAW_MAX);
    app_state_build_day(s_ui.raw, s_ui.raw_count, key, &s_ui.day);
    render_day();
}

// 列表页双向循环切换日期，按聚合视图的顺序浏览赛程。
static bool shift_day(int delta) {
    if (s_ui.dates.count == 0) return false;
    int next = s_ui.dates.selected + delta;
    if (next < 0 || next >= s_ui.dates.count) return false;
    s_ui.dates.selected = next;
    enter_day(s_ui.dates.dates[next].key);
    return true;
}

// 比赛与日期组成一条连续列表；跨过首尾比赛时切到相邻日期。
static void move_list(int delta) {
    if (s_ui.day.count == 0) {
        shift_day(delta);
        return;
    }
    int next = s_ui.day.selected + delta;
    if (next >= 0 && next < s_ui.day.count) {
        s_ui.day.selected = next;
        render_day();
        return;
    }
    if (shift_day(delta) && delta < 0 && s_ui.day.count > 0) {
        s_ui.day.selected = s_ui.day.count - 1;
        render_day();
    }
}

// 从详情页返回当日页:重建并尽量恢复选中位置。
static void return_to_day(void) {
    char key[APP_DATE_KEY_MAX];
    int back = s_ui.day.selected;
    snprintf(key, sizeof(key), "%s", s_ui.day.key);
    s_ui.page = PAGE_DAY;
    s_ui.data_version = app_data_last_ok_ms();
    s_ui.raw_count = app_data_get_matches(s_ui.raw, APP_RAW_MAX);
    app_state_build_day(s_ui.raw, s_ui.raw_count, key, &s_ui.day);
    if (back >= 0 && back < s_ui.day.count) {
        s_ui.day.selected = back;
    }
    render_day();
}

// ================================================================ 详情页

// 48x48 深色圆角队徽底板
static lv_obj_t *make_logo_square(lv_obj_t *parent, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *sq = lv_obj_create(parent);
    lv_obj_set_pos(sq, x, y);
    lv_obj_set_size(sq, 48, 48);
    lv_obj_clear_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sq, 12, 0);
    lv_obj_set_style_bg_color(sq, lv_color_hex(C_LOGO_BG), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sq, 0, 0);
    lv_obj_set_style_pad_all(sq, 0, 0);
    return sq;
}

// 胜方队名下的金色下划线
static void make_win_underline(lv_coord_t cx, lv_coord_t y) {
    lv_obj_t *bar = lv_obj_create(s_ui.body);
    lv_obj_set_pos(bar, cx - 11, y);
    lv_obj_set_size(bar, 22, 2);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
}

static void build_detail_page(void) {
    lv_label_set_text(s_ui.lbl_hint, "OK 返回比赛列表 · 上下滚动");
    lv_obj_clean(s_ui.body);
    app_match_detail_t d = { 0 };
    bool has_detail = app_data_get_detail(s_ui.showing_detail, &d);
    if (!has_detail) {
        const app_match_t *summary = NULL;
        for (int i = 0; i < s_ui.raw_count; i++) {
            if (s_ui.raw[i].id == s_ui.showing_detail) {
                summary = &s_ui.raw[i];
                break;
            }
        }
        if (!summary) return;
        d.match = *summary;
        d.win_team = -1;
    }

    const app_match_t *m = &d.match;

    // === 第 1 行:V 标志 + 赛事名(同排) + 日期 ===
    const void *bdsc = app_brand_dsc();
    if (bdsc) {
        lv_obj_t *vct_img = lv_image_create(s_ui.body);
        lv_image_set_src(vct_img, bdsc);
        lv_obj_align(vct_img, LV_ALIGN_TOP_LEFT, 6, 3);
    }
    const char *ev = m->event[0] ? m->event : "VCT";
    // 形如 "2026无畏契约全球冠军赛":跳过开头 4 位年份,一行放得下
    if (ev[0] >= '0' && ev[0] <= '9' && ev[1] >= '0' && ev[1] <= '9' &&
        ev[2] >= '0' && ev[2] <= '9' && ev[3] >= '0' && ev[3] <= '9') {
        ev += 4;
    }
    lv_obj_t *tour = make_label(s_ui.body, &noto_sc_16, C_INK,
                                LV_ALIGN_TOP_LEFT, 34, 5);
    lv_obj_set_width(tour, 132);
    lv_label_set_long_mode(tour, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(tour, ev);

    // 右侧日期 mm/dd(start_ts 为 UTC,设备按 UTC+8 展示)
    struct tm tmv;
    gmtime_r((const time_t[]){ m->start_ts + 8 * 3600 }, &tmv);
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d/%02d",
             (int)tmv.tm_mon + 1, (int)tmv.tm_mday);
    lv_obj_t *date_lbl = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                    LV_ALIGN_TOP_RIGHT, -6, 5);
    lv_label_set_text(date_lbl, date_str);

    // === 第 2 行:队徽(深色圆角底板) + 中央大比分 + BO ===
    bool has0 = m->logo[0] >= 0 && app_logos_dsc_32(m->logo[0]) != NULL;
    bool has1 = m->logo[1] >= 0 && app_logos_dsc_32(m->logo[1]) != NULL;

    // 左队徽
    lv_obj_t *sq0 = make_logo_square(s_ui.body, 10, 32);
    if (has0) {
        lv_obj_t *i0 = lv_image_create(sq0);
        lv_image_set_src(i0, app_logos_dsc_32(m->logo[0]));
        lv_obj_center(i0);
    } else {
        lv_obj_t *fb = lv_label_create(sq0);
        lv_label_set_text(fb, m->sp[0][0] ? m->sp[0] : "?");
        lv_obj_set_style_text_color(fb, lv_color_hex(C_SUB), 0);
        lv_obj_set_style_text_font(fb, &noto_sc_16, 0);
        lv_obj_center(fb);
    }

    // 右队徽
    lv_obj_t *sq1 = make_logo_square(s_ui.body, 174, 32);
    if (has1) {
        lv_obj_t *i1 = lv_image_create(sq1);
        lv_image_set_src(i1, app_logos_dsc_32(m->logo[1]));
        lv_obj_center(i1);
    } else {
        lv_obj_t *fb = lv_label_create(sq1);
        lv_label_set_text(fb, m->sp[1][0] ? m->sp[1] : "?");
        lv_obj_set_style_text_color(fb, lv_color_hex(C_SUB), 0);
        lv_obj_set_style_text_font(fb, &noto_sc_16, 0);
        lv_obj_center(fb);
    }

    // 中央比分:两个数字(胜方金色) + 冒号,22px
    uint32_t c0 = (d.win_team == 0) ? C_GOLD : C_INK;
    uint32_t c1 = (d.win_team == 1) ? C_GOLD : C_INK;
    lv_obj_t *s0 = make_label(s_ui.body, &noto_sc_22, c0,
                              LV_ALIGN_TOP_LEFT, 66, 40);
    lv_obj_set_width(s0, 42);
    lv_obj_set_style_text_align(s0, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(s0, "%d", m->score[0]);
    lv_obj_t *colon = make_label(s_ui.body, &noto_sc_22, C_SUB,
                                 LV_ALIGN_TOP_LEFT, 110, 40);
    lv_obj_set_width(colon, 20);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(colon, ":");
    lv_obj_t *s1 = make_label(s_ui.body, &noto_sc_22, c1,
                              LV_ALIGN_TOP_LEFT, 132, 40);
    lv_obj_set_width(s1, 42);
    lv_obj_set_style_text_align(s1, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text_fmt(s1, "%d", m->score[1]);

    // BO 位于比分下方居中
    lv_obj_t *bo_lbl = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                  LV_ALIGN_TOP_LEFT, 96, 76);
    lv_obj_set_width(bo_lbl, 48);
    lv_obj_set_style_text_align(bo_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(bo_lbl, m->bo ? (m->bo == 1 ? "BO1" : "") : "");
    if (m->bo && m->bo > 1) lv_label_set_text_fmt(bo_lbl, "BO%d", m->bo);

    // 队名(队徽正下方),胜方金色下划线
    lv_obj_t *n0 = make_label(s_ui.body, &noto_sc_16, C_INK,
                              LV_ALIGN_TOP_LEFT, 0, 84);
    lv_obj_set_pos(n0, 0, 84);
    lv_obj_set_width(n0, 68);
    lv_obj_set_style_text_align(n0, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(n0, m->sp[0][0] ? m->sp[0] : "待定");
    if (d.win_team == 0) make_win_underline(34, 106);

    lv_obj_t *n1 = make_label(s_ui.body, &noto_sc_16, C_INK,
                                  LV_ALIGN_TOP_LEFT, 156, 84);
    lv_obj_set_width(n1, 68);
    lv_obj_set_style_text_align(n1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(n1, m->sp[1][0] ? m->sp[1] : "待定");
    if (d.win_team == 1) make_win_underline(190, 106);

    // === 分隔线 ===
    lv_obj_t *div = lv_obj_create(s_ui.body);
    lv_obj_set_pos(div, 12, 112);
    lv_obj_set_size(div, 216, 1);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(div, lv_color_hex(C_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);

    // === 逐图行:地图图标 + 名称 + 比分 ===
    int shown = 0;
    lv_coord_t row_y = 116;
    for (int i = 0; i < d.map_count && i < APP_MAP_MAX; i++) {
        const app_map_t *mp = &d.maps[i];
        if (!mp->have) continue;

        // 地图五边形小图标
        lv_coord_t name_x = 16;
        const void *midsc = app_map_icons_dsc(mp->icon);
        if (midsc) {
            lv_obj_t *ico = lv_image_create(s_ui.body);
            lv_image_set_src(ico, midsc);
            lv_obj_set_pos(ico, 14, row_y + 6);
            name_x = 42;
        }

        // 地图名
        lv_obj_t *name = make_label(s_ui.body, &noto_sc_16, C_INK,
                                    LV_ALIGN_TOP_LEFT, name_x, row_y + 8);
        lv_obj_set_width(name, 96);
        lv_label_set_text(name, mp->name);

        // 右侧比分:胜方金色
        if (mp->rounds > 0) {
            uint32_t ma = (mp->win == 0) ? C_GOLD : C_INK;
            uint32_t mb = (mp->win == 1) ? C_GOLD : C_INK;
            lv_obj_t *ma_lbl = make_label(s_ui.body, &noto_sc_16, ma,
                                          LV_ALIGN_TOP_LEFT, 148, row_y + 8);
            lv_obj_set_width(ma_lbl, 30);
            lv_obj_set_style_text_align(ma_lbl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_text_fmt(ma_lbl, "%d", mp->score[0]);
            lv_obj_t *mc = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                      LV_ALIGN_TOP_LEFT, 180, row_y + 8);
            lv_obj_set_width(mc, 8);
            lv_obj_set_style_text_align(mc, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(mc, ":");
            lv_obj_t *mb_lbl = make_label(s_ui.body, &noto_sc_16, mb,
                                          LV_ALIGN_TOP_LEFT, 190, row_y + 8);
            lv_obj_set_width(mb_lbl, 30);
            lv_obj_set_style_text_align(mb_lbl, LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_text_fmt(mb_lbl, "%d", mp->score[1]);
        } else {
            lv_obj_t *wait_lbl = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                            LV_ALIGN_TOP_LEFT, 180, row_y + 8);
            lv_obj_set_width(wait_lbl, 40);
            lv_obj_set_style_text_align(wait_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(wait_lbl, "待赛");
        }

        row_y += 32;
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *none = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                    LV_ALIGN_TOP_MID, 0, 150);
        lv_label_set_text(none, m->status == APP_STATUS_UPCOMING
                          ? "比赛尚未开始\n暂无逐图数据"
                          : "逐图数据暂不可用\n稍后自动重试");
    }
}

// ================================================================ 配网页

static void build_prov_page(void) {
    lv_obj_clean(s_ui.body);

    lv_obj_t *title = make_label(s_ui.body, &noto_sc_22, C_INK,
                                 LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(title, "Wi-Fi 配网");

    int state = app_wifi_verify_state();
    if (state == 1 || state == 2) {
        int progress = app_wifi_verify_progress();
        lv_obj_t *stage = make_label(s_ui.body, &noto_sc_16,
                                     state == 2 ? C_GOLD : C_SUB,
                                     LV_ALIGN_TOP_MID, 0, 62);
        lv_label_set_text(stage, state == 2 ? "配网成功，正在同步赛程"
                          : progress < 30 ? "正在扫描目标网络"
                          : progress < 90 ? "正在连接路由器" : "正在保存配置");
        lv_obj_t *bar = lv_bar_create(s_ui.body);
        lv_obj_set_size(bar, 204, 12);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 104);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, progress, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_DIVIDER), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(state == 2 ? C_GOLD : C_LIVE),
                                  LV_PART_INDICATOR);
        lv_obj_t *pct = make_label(s_ui.body, &noto_sc_22, C_INK,
                                   LV_ALIGN_TOP_MID, 0, 128);
        lv_label_set_text_fmt(pct, "%d%%", progress);
        lv_obj_t *tip = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                   LV_ALIGN_TOP_MID, 0, 184);
        lv_obj_set_width(tip, 216);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(tip, state == 2 ? "手机可查看成功反馈\n即将进入比赛列表"
                          : "看板热点保持开启\n手机页面同步显示进度");
        return;
    }

    int fr = app_wifi_last_fail_reason();
    if (state == 3) {
        // 失败诊断放屏幕顶部、大字号,保证一眼可见
        char msg[48];
        snprintf(msg, sizeof(msg), "连接失败 原因码:%d", fr);
        lv_obj_t *warn = make_label(s_ui.body, &noto_sc_22, C_LIVE,
                                    LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_width(warn, 228);
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(warn, msg);

        int pre, pin, post;
        app_wifi_diag_info(&pre, &pin, &post);
        char msg2[80];
        snprintf(msg2, sizeof(msg2), "预扫:%d 锁定CH%d 复查:%d", pre, pin, post);
        lv_obj_t *d1 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                  LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_set_style_text_align(d1, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(d1, msg2);

        char tgt[40];
        int tlen = app_wifi_get_target_ssid(tgt, sizeof(tgt));
        char msg3[80];
        snprintf(msg3, sizeof(msg3), "目标:%s (%d字节)", tgt, tlen);
        lv_obj_t *d2 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                  LV_ALIGN_TOP_MID, 0, 112);
        lv_obj_set_width(d2, 216);
        lv_obj_set_style_text_align(d2, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(d2, msg3);

        char mac[24];
        app_wifi_diag_pin_mac(mac, sizeof(mac));
        if (mac[0]) {
            lv_obj_t *d3 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                      LV_ALIGN_TOP_MID, 0, 134);
            lv_obj_set_style_text_align(d3, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(d3, mac);
        }

        const char *strats = app_wifi_diag_strats();
        if (strats[0]) {
            lv_obj_t *d4 = make_label(s_ui.body, &noto_sc_16, C_UPCOMING,
                                      LV_ALIGN_TOP_MID, 0, 156);
            lv_obj_set_style_text_align(d4, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(d4, strats);
        }

        lv_obj_t *step1 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                     LV_ALIGN_TOP_LEFT, 12, 186);
        lv_obj_set_width(step1, 216);
        lv_label_set_text(step1, "重试:手机连接热点");

        char ap[40];
        app_wifi_get_ap_ssid(ap, sizeof(ap));
        if (ap[0] == '\0') snprintf(ap, sizeof(ap), "VCTBoard-****");
        lv_obj_t *ssid = make_label(s_ui.body, &noto_sc_22, C_UPCOMING,
                                    LV_ALIGN_TOP_MID, 0, 210);
        lv_label_set_text(ssid, ap);

        lv_obj_t *step2 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                     LV_ALIGN_TOP_LEFT, 12, 248);
        lv_obj_set_width(step2, 216);
        lv_label_set_text(step2, "浏览器打开: 192.168.4.1");
        return;
    }

    lv_obj_t *step1 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                 LV_ALIGN_TOP_LEFT, 12, 56);
    lv_obj_set_width(step1, 216);
    lv_label_set_text(step1, "1. 手机连接看板热点:");

    char ap[40];
    app_wifi_get_ap_ssid(ap, sizeof(ap));
    if (ap[0] == '\0') snprintf(ap, sizeof(ap), "VCTBoard-****");
    lv_obj_t *ssid = make_label(s_ui.body, &noto_sc_22, C_UPCOMING,
                                LV_ALIGN_TOP_MID, 0, 82);
    lv_label_set_text(ssid, ap);

    lv_obj_t *step2 = make_label(s_ui.body, &noto_sc_16, C_SUB,
                                 LV_ALIGN_TOP_LEFT, 12, 122);
    lv_obj_set_width(step2, 216);
    lv_label_set_text(step2, "2. 浏览器打开:");

    lv_obj_t *url = make_label(s_ui.body, &noto_sc_22, C_UPCOMING,
                               LV_ALIGN_TOP_MID, 0, 148);
    lv_label_set_text(url, "192.168.4.1");

    lv_obj_t *tip = make_label(s_ui.body, &noto_sc_16, C_SUB,
                               LV_ALIGN_TOP_LEFT, 12, 196);
    lv_obj_set_width(tip, 216);
    lv_label_set_text(tip, "填入家庭 Wi-Fi 名称与密码后,\n看板自动连接并同步赛程。");
}

// ================================================================ 启动页

static void build_boot_page(void) {
    lv_obj_clean(s_ui.body);
    app_data_status_t st = app_data_status();

    const char *text;
    uint32_t color = C_SUB;
    if (app_wifi_is_sta_connected()) {
        if (st == APP_DATA_OK) {
            text = "已同步";
            color = C_WIN;
        } else if (st == APP_DATA_ERR) {
            text = "获取赛程失败\n稍后自动重试…";
            color = C_LIVE;
        } else {
            text = "连接成功\n正在获取赛程…";
        }
    } else if (app_wifi_is_provisioning()) {
        build_prov_page();
        s_ui.page = PAGE_PROV;
        return;
    } else {
        text = "Wi-Fi 连接中…";
    }

    lv_obj_t *l = make_label(s_ui.body, &noto_sc_16, color,
                             LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_width(l, 220);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);

    // 屏幕版本标记(确认刷入的是哪版固件)
    lv_obj_t *tag = make_label(s_ui.body, &noto_sc_16, C_SUB,
                               LV_ALIGN_TOP_MID, 0, 210);
    lv_label_set_text(tag, APP_FW_TAG);
}

// ================================================================ 页面切换

static void goto_page(page_t p) {
    s_ui.page = p;
    switch (p) {
    case PAGE_BOOT:
        build_boot_page();
        break;
    case PAGE_DATES:
        build_dates_page();
        if (s_ui.dates.count > 0) {
            enter_day(s_ui.dates.dates[s_ui.dates.selected].key);
        }
        break;
    case PAGE_DAY:
        render_day();
        break;
    case PAGE_DETAIL:
        build_detail_page();
        break;
    case PAGE_PROV:
        build_prov_page();
        break;
    }
}

// ================================================================ 定时器

static void ui_timer_cb(lv_timer_t *timer) {
    (void)timer;
    refresh_clock(false);

    // 电量每 ~30s 刷一次
    static int batt_tick = 0;
    if (++batt_tick >= 60) {
        batt_tick = 0;
        refresh_batt();
    }

    switch (s_ui.page) {
    case PAGE_BOOT: {
        if (app_wifi_is_sta_connected() ||
            (app_data_status() == APP_DATA_OK && app_data_last_ok_ms() > 0)) {
            goto_page(PAGE_DATES);
        } else if (app_data_status() == APP_DATA_ERR ||
                   app_wifi_is_provisioning()) {
            build_boot_page();          // 刷新状态文本/转配网
        }
        break;
    }
    case PAGE_DATES: {
        // 仅数据变化时重绘，避免每秒重建整页导致按键响应迟钝。
        int64_t v = app_data_last_ok_ms();
        app_data_status_t status = app_data_status();
        bool time_ready = app_wifi_sntp_synced();
        if (v != s_ui.data_version || status != s_ui.data_status_seen ||
            time_ready != s_ui.time_ready_seen) {
            build_dates_data(true);
            s_ui.data_version = v;
            s_ui.data_status_seen = status;
            s_ui.time_ready_seen = time_ready;
            if (s_ui.dates.count > 0) {
                enter_day(s_ui.dates.dates[s_ui.dates.selected].key);
            } else {
                render_dates();
            }
        }
        break;
    }
    case PAGE_DAY: {
        int64_t v = app_data_last_ok_ms();
        if (v != s_ui.data_version) {
            build_dates_data(true);
            return_to_day();
        }
        break;
    }
    case PAGE_PROV: {
        static int shown_state = -1;
        static int shown_progress = -1;
        static int success_ticks = 0;
        int state = app_wifi_verify_state();
        int progress = app_wifi_verify_progress();
        if (state != shown_state || progress != shown_progress) {
            shown_state = state;
            shown_progress = progress;
            goto_page(PAGE_PROV);
        }
        if (state == 2 && app_wifi_is_sta_connected()) {
            if (++success_ticks >= 4) {
                success_ticks = 0;
                goto_page(PAGE_DATES);
            }
        } else {
            success_ticks = 0;
        }
        break;
    }
    case PAGE_DETAIL: {
        // 已结束/直播比赛的详情失败时低频重试；未开始只展示赛前摘要。
        static unsigned retry_tick = 0;
        app_match_detail_t d;
        bool ready = app_data_get_detail(s_ui.showing_detail, &d);
        if (ready && !s_ui.detail_ready) {
            build_detail_page();
        } else if (!ready && ++retry_tick >= 20) {
            retry_tick = 0;
            for (int i = 0; i < s_ui.raw_count; i++) {
                if (s_ui.raw[i].id == s_ui.showing_detail &&
                    s_ui.raw[i].status != APP_STATUS_UPCOMING) {
                    app_data_request_detail(s_ui.showing_detail);
                    break;
                }
            }
        }
        s_ui.detail_ready = ready;
        break;
    }
    default:
        break;
    }
}

// ================================================================ 按键

void app_ui_key(app_ui_key_t key) {
    switch (s_ui.page) {
    case PAGE_BOOT:
        break;

    case PAGE_DATES:
        if (key == APP_UI_KEY_UP) {
            if (s_ui.dates.selected > 0) {
                s_ui.dates.selected--;
                render_dates();
            }
        } else if (key == APP_UI_KEY_DOWN) {
            if (s_ui.dates.selected < s_ui.dates.count - 1) {
                s_ui.dates.selected++;
                render_dates();
            }
        } else if (key == APP_UI_KEY_OK) {
            if (s_ui.dates.selected >= 0 &&
                s_ui.dates.selected < s_ui.dates.count) {
                enter_day(s_ui.dates.dates[s_ui.dates.selected].key);
            }
        } else if (key == APP_UI_KEY_OK_LONG) {
            if (!app_data_is_preview()) {
                app_wifi_start_provisioning();
                goto_page(PAGE_PROV);
            }
        }
        break;

    case PAGE_DAY:
        if (key == APP_UI_KEY_UP) {
            move_list(-1);
        } else if (key == APP_UI_KEY_DOWN) {
            move_list(1);
        } else if (key == APP_UI_KEY_OK) {
            if (s_ui.day.selected >= 0 &&
                s_ui.day.selected < s_ui.day.count) {
                const app_match_t *sel =
                    &s_ui.raw[s_ui.day.raw_idx[s_ui.day.selected]];
                s_ui.showing_detail = sel->id;
                s_ui.detail_ready = false;
                if (sel->status != APP_STATUS_UPCOMING) {
                    app_data_request_detail(s_ui.showing_detail);
                }
                goto_page(PAGE_DETAIL);
            }
        } else if (key == APP_UI_KEY_OK_LONG) {
            if (!app_data_is_preview()) {
                app_wifi_start_provisioning();
                goto_page(PAGE_PROV);
            }
        }
        break;

    case PAGE_DETAIL:
        if (key == APP_UI_KEY_OK || key == APP_UI_KEY_OK_LONG) {
            return_to_day();
        } else if (key == APP_UI_KEY_UP || key == APP_UI_KEY_DOWN) {
            lv_obj_scroll_by(s_ui.body, 0, key == APP_UI_KEY_UP ? 52 : -52,
                             LV_ANIM_ON);
        }
        break;

    case PAGE_PROV:
        if (key == APP_UI_KEY_OK_LONG || key == APP_UI_KEY_OK) {
            // 无存档凭据时不能离开配网页(否则无法恢复)
            goto_page(PAGE_DATES);
        }
        break;
    }
}

// ================================================================ 初始化

void app_ui_init(void) {
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.dates.selected = -1;
    s_ui.day.selected = -1;
    s_ui.last_batt = -1;
    s_ui.last_clock_min = -1;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶栏
    s_ui.topbar = lv_obj_create(scr);
    lv_obj_set_size(s_ui.topbar, 240, 30);
    lv_obj_clear_flag(s_ui.topbar, LV_OBJ_FLAG_SCROLLABLE);
    style_card(s_ui.topbar, C_PANEL);
    lv_obj_set_style_radius(s_ui.topbar, 0, 0);
    lv_obj_align(s_ui.topbar, LV_ALIGN_TOP_MID, 0, 0);

    // 顶栏左上无畏契约红色 V 标志(所有页面常驻)
    lv_obj_t *brand = lv_image_create(s_ui.topbar);
    lv_image_set_src(brand, app_brand_dsc());
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 7, 0);

    s_ui.lbl_title = make_label(s_ui.topbar, &noto_sc_16, C_INK,
                                LV_ALIGN_LEFT_MID, 36, 0);
    lv_label_set_text(s_ui.lbl_title, "VCT 赛程");
    s_ui.lbl_clock = make_label(s_ui.topbar, &noto_sc_16, C_SUB,
                                LV_ALIGN_RIGHT_MID, -44, 0);
    lv_label_set_text(s_ui.lbl_clock, "--:--");
    s_ui.lbl_batt = make_label(s_ui.topbar, &noto_sc_16, C_SUB,
                               LV_ALIGN_RIGHT_MID, -6, 0);
    lv_label_set_text(s_ui.lbl_batt, "");

    // 主体滚动容器
    s_ui.body = lv_obj_create(scr);
    lv_obj_set_size(s_ui.body, 240, 264);
    lv_obj_align(s_ui.body, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_ui.body, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_ui.body, 0, 0);
    lv_obj_set_style_pad_all(s_ui.body, 8, 0);
    lv_obj_set_scroll_dir(s_ui.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.body, LV_SCROLLBAR_MODE_AUTO);

    // 底部提示
    s_ui.lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_ui.lbl_hint, &noto_sc_16, 0);
    lv_obj_set_style_text_color(s_ui.lbl_hint, lv_color_hex(C_SUB), 0);
    lv_obj_align(s_ui.lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_ui.lbl_hint, "上下选日期 · OK 进入 · 长按配网");

    refresh_batt();
    refresh_clock(true);
    goto_page(PAGE_BOOT);

    lv_timer_create(ui_timer_cb, 1000, NULL);
}
