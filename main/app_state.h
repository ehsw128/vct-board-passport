// main/app_state.h —— 赛程分类/排序/展示窗口与时间工具(纯逻辑,可 host 测试)。
//
// 展示窗口规则:
//   进行中   matchStatusId==2,按开赛时间升序,最多 APP_MAX_LIVE 场
//   即将开始 未开打且开赛时间在未来,升序,最多 APP_MAX_UPCOMING 场
//   最近结束 已结束,按开赛时间倒序,最多 APP_MAX_DONE 场
// 分组内平铺成一条可上下移动的选中列表,顺序 固定为 进行中->即将->已结束。
#pragma once

#include "app_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MAX_LIVE       8
#define APP_MAX_UPCOMING   6
#define APP_MAX_DONE       4

// ---- 三层导航:日期视图 / 当日比赛视图 ----
#define APP_DATES_MAX      32
#define APP_DAY_MAX        10
#define APP_DATE_KEY_MAX   11   // "YYYY-MM-DD" + NUL

typedef enum {
    APP_GROUP_TODAY = 0,
    APP_GROUP_PAST,
    APP_GROUP_FUTURE,
} app_group_t;

typedef struct {
    char key[APP_DATE_KEY_MAX];   // 当地日期 "YYYY-MM-DD"
    int16_t wday;                 // 星期 0=周日
    int16_t match_count;
    int16_t has_live;
    int16_t has_upcoming;
    int16_t has_done;
    app_group_t group;
} app_date_entry_t;

typedef struct {
    app_date_entry_t dates[APP_DATES_MAX];
    int count;
    int selected;                 // -1=无
} app_dates_view_t;

typedef struct {
    int raw_idx[APP_DAY_MAX];
    int count;
    int selected;
    char key[APP_DATE_KEY_MAX];
} app_day_view_t;

// 由原始赛程构建日期聚合视图:仅含有比赛的日期。
// 排序:今天优先 -> 过去日期倒序(最近在前) -> 未来日期升序(最近在前)。
void app_state_build_dates(const app_match_t *raw, int raw_count,
                           int64_t now, app_dates_view_t *out);

// 由指定日期键("YYYY-MM-DD")过滤当日比赛,按开赛时间升序。
void app_state_build_day(const app_match_t *raw, int raw_count,
                         const char *key, app_day_view_t *out);

typedef enum {
    APP_SECTION_LIVE = 0,
    APP_SECTION_UPCOMING,
    APP_SECTION_DONE,
} app_section_t;

typedef struct {
    int raw_idx;             // 指回原始数组的下标
    app_section_t section;
} app_view_item_t;

typedef struct {
    app_view_item_t items[APP_VIEW_MAX];
    int count;
    int selected;            // -1 = 无选中
} app_view_t;

// 由原始赛程数组构建展示视图(分类 + 排序 + 截断)。
void app_state_build_view(const app_match_t *raw, int raw_count, app_view_t *out);

// 选中项移动,delta=+1/-1;越界则钳制在边界。
void app_state_move(app_view_t *view, int delta);

// 解析 "2026-09-27T17:00:00+08:00" 为 epoch 秒;失败返回 0。
// 只处理 +HH:MM / -HH:MM / Z 三种时区形态(接口实测只有第一种)。
int64_t app_state_parse_iso8601(const char *s);

// "BO3" -> 3;"BO1"/空 -> 1;其他 -> 0。
int app_state_parse_bo(const char *s);

// 时间显示:
//   今天 -> "今天 17:00"   明天 -> "明天 17:00"
//   同年其他 -> "9/30 17:00"   跨年 -> "2027/1/2 17:00"
//   无效 -> "--:--"
void app_state_fmt_time(int64_t ts, int64_t now, char *buf, int buf_size);

#ifdef __cplusplus
}
#endif
