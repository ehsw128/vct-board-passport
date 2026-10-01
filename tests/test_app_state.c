// tests/test_app_state.c —— 赛程视图/时间工具 host 测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"

static app_match_t mk(int64_t id, int status, int64_t ts) {
    app_match_t m;
    memset(&m, 0, sizeof(m));
    m.have = true;
    m.id = id;
    m.status = status;
    m.start_ts = ts;
    m.bo = 3;
    snprintf(m.sp[0], APP_SP_MAX, "A%lld", (long long)id);
    snprintf(m.sp[1], APP_SP_MAX, "B%lld", (long long)id);
    return m;
}

int main(void) {
    // ---- ISO8601 解析 ----
    // 2026-09-27 17:00:00 +08:00 == 2026-09-27 09:00:00 UTC
    assert(app_state_parse_iso8601("2026-09-27T17:00:00+08:00") ==
           1790499600);
    assert(app_state_parse_iso8601("2026-09-27T09:00:00Z") == 1790499600);
    assert(app_state_parse_iso8601("2026-09-27T01:00:00-08:00") ==
           1790499600);
    assert(app_state_parse_iso8601("") == 0);
    assert(app_state_parse_iso8601("garbage") == 0);

    // ---- BO 解析 ----
    assert(app_state_parse_bo("BO3") == 3);
    assert(app_state_parse_bo("BO5") == 5);
    assert(app_state_parse_bo("BO1") == 1);
    assert(app_state_parse_bo("") == 0);
    assert(app_state_parse_bo("XO7") == 0);

    // ---- 视图构建:2 直播 + 10 未开 + 8 已结束 ----
    app_match_t raw[APP_RAW_MAX];
    memset(raw, 0, sizeof(raw));
    int64_t base = 1790499600;           // 2026-09-27 17:00 +08
    int n = 0;

    // 已结束:8 场,时间 9/25~9/26(倒序应取最近的 4 场)
    for (int i = 0; i < 8; i++) {
        raw[n++] = mk(900 + i, APP_STATUS_DONE, base - (8 - i) * 3600);
    }
    // 直播:2 场
    raw[n++] = mk(700, APP_STATUS_LIVE, base - 3600);
    raw[n++] = mk(701, APP_STATUS_LIVE, base - 1800);
    // 未开打:10 场,时间 9/28 17:00 起(升序取前 6)
    for (int i = 0; i < 10; i++) {
        raw[n++] = mk(800 + i, APP_STATUS_UPCOMING, base + (i + 1) * 7200);
    }

    app_view_t view;
    app_state_build_view(raw, n, &view);

    assert(view.count == 2 + 6 + 4);
    assert(view.selected == 0);
    // 分组顺序:LIVE -> UPCOMING -> DONE
    assert(view.items[0].section == APP_SECTION_LIVE);
    assert(view.items[1].section == APP_SECTION_LIVE);
    assert(view.items[2].section == APP_SECTION_UPCOMING);
    assert(view.items[view.count - 1].section == APP_SECTION_DONE);
    // 直播按开赛时间升序
    assert(raw[view.items[0].raw_idx].id == 700);
    assert(raw[view.items[1].raw_idx].id == 701);
    // 未开打升序取最近 6 场:800..805
    for (int i = 0; i < 6; i++) {
        assert(view.items[2 + i].section == APP_SECTION_UPCOMING);
        assert(raw[view.items[2 + i].raw_idx].id == 800 + i);
    }
    // 已结束倒序取最近 4 场:组首最新 907,组尾最旧 904
    assert(raw[view.items[view.count - 4].raw_idx].id == 907);
    assert(raw[view.items[view.count - 1].raw_idx].id == 904);

    // ---- 选中移动与钳制 ----
    app_state_move(&view, 1);
    assert(view.selected == 1);
    view.selected = view.count - 1;
    app_state_move(&view, 1);            // 触底不动
    assert(view.selected == view.count - 1);
    app_state_move(&view, -100);         // 触顶钳到 0
    assert(view.selected == 0);
    app_state_move(&view, -1);           // 已在 0,保持
    assert(view.selected == 0);

    // ---- 空视图 ----
    app_view_t empty_view;
    app_state_build_view(raw, 0, &empty_view);
    assert(empty_view.count == 0 && empty_view.selected == -1);
    app_state_move(&empty_view, 1);
    assert(empty_view.selected == -1);

    // ---- 时间格式化(固定东八区) ----
    char buf[32];
    // 当天 17:00,now 为当天 10:00(+08)
    app_state_fmt_time(base, base - 7 * 3600, buf, sizeof(buf));
    assert(strcmp(buf, "今天 17:00") == 0);
    // 明天
    app_state_fmt_time(base + 86400, base - 7 * 3600, buf, sizeof(buf));
    assert(strcmp(buf, "明天 17:00") == 0);
    // 同年更早:9/25
    app_state_fmt_time(base - 2 * 86400, base, buf, sizeof(buf));
    assert(strcmp(buf, "9/25 17:00") == 0);
    // 无效时间
    app_state_fmt_time(0, base, buf, sizeof(buf));
    assert(strcmp(buf, "--:--") == 0);

    printf("test_app_state: PASS\n");
    return 0;
}
