// main/app_state.c —— 展示视图构建与时间工具。不依赖 ESP-IDF,可在主机上测试。
#include "app_state.h"

#include <string.h>
#include <stdio.h>

// 公历天数 -> epoch 秒(Henry S. Warren 的 days_from_civil 算法,
// 不依赖平台的 timegm)。
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);            // [0, 399]
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t app_state_parse_iso8601(const char *s) {
    if (!s) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    // 2026-09-27T17:00:00+08:00
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return 0;
    }
    int64_t epoch = days_from_civil(y, mo, d) * 86400 +
                    h * 3600 + mi * 60 + sec;
    // 时区偏移:把本地时刻换算回 UTC(epoch 本身与时区无关,再减去偏移)
    const char *tz = strchr(s + 11, '+');
    bool neg = false;
    if (!tz) {
        tz = strchr(s + 11, '-');
        neg = true;
    }
    if (tz && tz[1] >= '0' && tz[3] >= '0') {
        int oh = (tz[1] - '0') * 10 + (tz[2] - '0');
        int om = (tz[4] - '0') * 10 + (tz[5] - '0');
        int off = oh * 3600 + om * 60;
        epoch += neg ? off : -off;
    }
    return epoch;
}

int app_state_parse_bo(const char *s) {
    if (!s) return 0;
    if (strncmp(s, "BO", 2) != 0) return 0;
    int n = s[2] - '0';
    if (n < 1 || n > 9) return 0;
    return n;
}

// 当地日历分解(时区固定东八区,设备上 SNTP 同步后的 epoch 与之一致;
// 主机测试同样按 +8 断言,避免依赖 setenv(TZ) 的平台差异)。
static void local_civil(int64_t ts, int *y, int *mo, int *d,
                        int *h, int *mi) {
    int64_t local = ts + 8 * 3600;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    // civil_from_days(同 Warren 算法)
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    if (mm <= 2) yy += 1;

    *y = (int)yy;
    *mo = (int)mm;
    *d = (int)dd;
    *h = (int)(rem / 3600);
    *mi = (int)((rem % 3600) / 60);
}

void app_state_fmt_time(int64_t ts, int64_t now, char *buf, int buf_size) {
    if (ts <= 0) {
        snprintf(buf, buf_size, "--:--");
        return;
    }
    int y, mo, d, h, mi;
    local_civil(ts, &y, &mo, &d, &h, &mi);
    int ny, nmo, nd, nh, nmi;
    local_civil(now, &ny, &nmo, &nd, &nh, &nmi);

    // 当地当天零点的 epoch
    int64_t midnight_now = (now + 8 * 3600) / 86400 * 86400 - 8 * 3600;
    int64_t day_diff = (ts - midnight_now) / 86400;

    if (day_diff == 0) {
        snprintf(buf, buf_size, "今天 %02d:%02d", h, mi);
    } else if (day_diff == 1) {
        snprintf(buf, buf_size, "明天 %02d:%02d", h, mi);
    } else if (y == ny) {
        snprintf(buf, buf_size, "%d/%d %02d:%02d", mo, d, h, mi);
    } else {
        snprintf(buf, buf_size, "%d/%d/%d %02d:%02d", y, mo, d, h, mi);
    }
    (void)nmo; (void)nd; (void)nh; (void)nmi;
}

static void push_item(app_view_t *v, int raw_idx, app_section_t section) {
    if (v->count >= APP_VIEW_MAX) return;
    v->items[v->count].raw_idx = raw_idx;
    v->items[v->count].section = section;
    v->count++;
}

// 稳定按 start_ts 升序插入(小规模,直接线性插入)。
static void insert_sorted_asc(app_match_t *arr, const app_match_t *m) {
    int i = 0;
    while (i < APP_MAX_UPCOMING && arr[i].have && arr[i].start_ts <= m->start_ts) {
        i++;
    }
    if (i >= APP_MAX_UPCOMING) return;          // 比已保留的最晚还晚:丢弃
    for (int j = APP_MAX_UPCOMING - 1; j > i; j--) arr[j] = arr[j - 1];
    arr[i] = *m;
}

// 倒序插入(最近结束:最新在前)。
static void insert_sorted_desc(app_match_t *arr, const app_match_t *m) {
    int i = 0;
    while (i < APP_MAX_DONE && arr[i].have && arr[i].start_ts >= m->start_ts) {
        i++;
    }
    if (i >= APP_MAX_DONE) return;
    for (int j = APP_MAX_DONE - 1; j > i; j--) arr[j] = arr[j - 1];
    arr[i] = *m;
}

void app_state_build_view(const app_match_t *raw, int raw_count, app_view_t *out) {
    memset(out, 0, sizeof(*out));
    out->selected = raw_count > 0 ? 0 : -1;

    app_match_t upcoming[APP_MAX_UPCOMING];
    app_match_t done[APP_MAX_DONE];
    memset(upcoming, 0, sizeof(upcoming));
    memset(done, 0, sizeof(done));
    int live_idx[APP_MAX_LIVE];
    int live_count = 0;

    for (int i = 0; i < raw_count; i++) {
        const app_match_t *m = &raw[i];
        if (!m->have || m->start_ts <= 0) continue;

        if (m->status == APP_STATUS_LIVE) {
            if (live_count < APP_MAX_LIVE) {
                // 按开赛时间升序插入
                int pos = live_count;
                while (pos > 0 && raw[live_idx[pos - 1]].start_ts > m->start_ts) {
                    live_idx[pos] = live_idx[pos - 1];
                    pos--;
                }
                live_idx[pos] = i;
                live_count++;
            }
        } else if (m->status == APP_STATUS_UPCOMING) {
            insert_sorted_asc(upcoming, m);
        } else if (m->status == APP_STATUS_DONE) {
            insert_sorted_desc(done, m);
        }
    }

    for (int i = 0; i < live_count; i++) {
        push_item(out, live_idx[i], APP_SECTION_LIVE);
    }
    for (int i = 0; i < APP_MAX_UPCOMING && upcoming[i].have; i++) {
        // 回填该窗口条目在原数组中的下标
        for (int k = 0; k < raw_count; k++) {
            if (raw[k].have && raw[k].id == upcoming[i].id) {
                push_item(out, k, APP_SECTION_UPCOMING);
                break;
            }
        }
    }
    for (int i = 0; i < APP_MAX_DONE && done[i].have; i++) {
        for (int k = 0; k < raw_count; k++) {
            if (raw[k].have && raw[k].id == done[i].id) {
                push_item(out, k, APP_SECTION_DONE);
                break;
            }
        }
    }
}

void app_state_move(app_view_t *view, int delta) {
    if (view->count == 0) {
        view->selected = -1;
        return;
    }
    int sel = view->selected + delta;
    if (sel < 0) sel = 0;
    if (sel >= view->count) sel = view->count - 1;
    view->selected = sel;
}

// ================================================================ 日期聚合

// 当地(UTC+8)日期键 "YYYY-MM-DD" 与星期(0=周日)。
static void local_date_key(int64_t ts_utc, char *key /*>=11*/, int *wday) {
    int y, mo, d, h, mi;
    local_civil(ts_utc, &y, &mo, &d, &h, &mi);
    snprintf(key, APP_DATE_KEY_MAX, "%04d-%02d-%02d", y, mo, d);
    // Warren 算法:days_from_civil 的零点是 1970-01-01,星期四(4)。
    int64_t days = days_from_civil(y, mo, d);
    *wday = (int)(((days + 4) % 7 + 7) % 7);
}

void app_state_build_dates(const app_match_t *raw, int raw_count,
                           int64_t now, app_dates_view_t *out) {
    memset(out, 0, sizeof(*out));
    out->selected = -1;

    char today_key[APP_DATE_KEY_MAX];
    int twd;
    local_date_key(now, today_key, &twd);

    for (int i = 0; i < raw_count; i++) {
        const app_match_t *m = &raw[i];
        if (!m->have || m->start_ts <= 0) continue;

        char key[APP_DATE_KEY_MAX];
        int wd;
        local_date_key(m->start_ts, key, &wd);

        app_date_entry_t *e = NULL;
        for (int k = 0; k < out->count; k++) {
            if (strcmp(out->dates[k].key, key) == 0) { e = &out->dates[k]; break; }
        }
        if (!e) {
            if (out->count >= APP_DATES_MAX) continue;
            e = &out->dates[out->count++];
            snprintf(e->key, sizeof(e->key), "%s", key);
            e->wday = (int16_t)wd;
            int c = strcmp(key, today_key);
            e->group = c < 0 ? APP_GROUP_PAST
                     : c > 0 ? APP_GROUP_FUTURE : APP_GROUP_TODAY;
        }
        e->match_count++;
        if (m->status == APP_STATUS_LIVE) e->has_live = 1;
        if (m->status == APP_STATUS_UPCOMING) e->has_upcoming = 1;
        if (m->status == APP_STATUS_DONE) e->has_done = 1;
    }

    // 严格按日历日期升序:下键只能去更晚的比赛，上键只能去更早的比赛。
    for (int i = 0; i < out->count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < out->count; j++) {
            if (strcmp(out->dates[j].key, out->dates[best].key) < 0) {
                best = j;
            }
        }
        if (best != i) {
            app_date_entry_t tmp = out->dates[i];
            out->dates[i] = out->dates[best];
            out->dates[best] = tmp;
        }
    }

    if (out->count == 0) return;
    // 默认选中:今天 -> 最近的未来 -> 最新的过去。
    out->selected = 0;
    bool found = false;
    for (int i = 0; i < out->count; i++) {
        if (out->dates[i].group == APP_GROUP_TODAY ||
            out->dates[i].group == APP_GROUP_FUTURE) {
            out->selected = i;
            found = true;
            break;
        }
    }
    if (!found && out->count > 0) out->selected = out->count - 1;
}

void app_state_build_day(const app_match_t *raw, int raw_count,
                         const char *key, app_day_view_t *out) {
    memset(out, 0, sizeof(*out));
    out->selected = -1;
    if (key) snprintf(out->key, sizeof(out->key), "%s", key);

    for (int i = 0; i < raw_count; i++) {
        if (!raw[i].have || raw[i].start_ts <= 0) continue;
        char dk[APP_DATE_KEY_MAX];
        int wd;
        local_date_key(raw[i].start_ts, dk, &wd);
        if (strcmp(dk, key) != 0) continue;
        if (out->count >= APP_DAY_MAX) break;

        // 按 start_ts 升序插入
        int pos = out->count;
        while (pos > 0 && raw[out->raw_idx[pos - 1]].start_ts > raw[i].start_ts) {
            out->raw_idx[pos] = out->raw_idx[pos - 1];
            pos--;
        }
        out->raw_idx[pos] = i;
        out->count++;
    }
    if (out->count > 0) out->selected = 0;
}
