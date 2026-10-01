// tests/test_app_dates.c —— 日期聚合与当日过滤的 host 测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"

static app_match_t mk(int day, int status, int hh, int mm, int score0, int score1) {
    app_match_t m;
    memset(&m, 0, sizeof(m));
    m.have = true;
    m.status = status;
    m.score[0] = score0;
    m.score[1] = score1;
    // 2026-09-{day}T{hh}:{mm}:00+08:00
    char s[40];
    snprintf(s, sizeof(s), "2026-09-%02dT%02d:%02d:00+08:00", day, hh, mm);
    m.id = (int64_t)day * 1000 + status;
    m.start_ts = app_state_parse_iso8601(s);
    return m;
}

int main(void) {
    app_match_t raw[8];
    // 9/24(过去)、9/25(过去)、9/29(今天,两场:一场已结束时刻一场未来时刻)、9/30(未来)
    raw[0] = mk(24, APP_STATUS_DONE, 17, 0, 2, 1);
    raw[1] = mk(25, APP_STATUS_DONE, 20, 0, 0, 2);
    raw[2] = mk(29, APP_STATUS_LIVE, 17, 0, 1, 1);
    raw[3] = mk(29, APP_STATUS_UPCOMING, 20, 0, 0, 0);
    raw[4] = mk(30, APP_STATUS_UPCOMING, 17, 0, 0, 0);

    // "现在"固定 2026-09-29 18:00 +08 -> UTC epoch
    int64_t now = app_state_parse_iso8601("2026-09-29T18:00:00+08:00");

    app_dates_view_t dv;
    app_state_build_dates(raw, 5, now, &dv);

    assert(dv.count == 4);
    // 始终按时间升序,上下键跨日期后也保持同一方向。
    assert(strcmp(dv.dates[0].key, "2026-09-24") == 0);
    assert(strcmp(dv.dates[1].key, "2026-09-25") == 0);
    assert(dv.dates[1].group == APP_GROUP_PAST);
    assert(strcmp(dv.dates[2].key, "2026-09-29") == 0);
    assert(dv.dates[2].group == APP_GROUP_TODAY);
    assert(dv.dates[2].match_count == 2);
    assert(dv.dates[2].has_live == 1);
    assert(strcmp(dv.dates[3].key, "2026-09-30") == 0);
    assert(dv.dates[3].group == APP_GROUP_FUTURE);

    // 默认选中今天
    assert(dv.selected == 2);

    // 当日过滤
    app_day_view_t day;
    app_state_build_day(raw, 5, "2026-09-29", &day);
    assert(day.count == 2);
    // 按开赛时间升序:17:00 在前
    assert(day.raw_idx[0] == 2);
    assert(day.raw_idx[1] == 3);
    assert(day.selected == 0);

    // 无比赛的日期
    app_state_build_day(raw, 5, "2026-09-28", &day);
    assert(day.count == 0);
    assert(day.selected == -1);

    // 没有今天/未来时选最新的过去;空列表保持无选中。
    app_state_build_dates(raw, 2, now, &dv);
    assert(dv.count == 2);
    assert(dv.selected == 1);
    app_state_build_dates(raw, 0, now, &dv);
    assert(dv.count == 0);
    assert(dv.selected == -1);

    printf("test_app_dates: PASS\n");
    return 0;
}
