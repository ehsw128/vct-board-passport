#include "app_preview.h"

#include <stdio.h>
#include <string.h>
#include "app_state.h"

int64_t app_preview_now(void) {
    return app_state_parse_iso8601("2026-09-29T19:00:00+08:00");
}

static void make_match(app_match_t *m, int64_t id, const char *when,
                       int status, const char *a, const char *b,
                       int score_a, int score_b, int bo) {
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->start_ts = app_state_parse_iso8601(when);
    m->status = status;
    m->score[0] = score_a;
    m->score[1] = score_b;
    m->bo = bo;
    snprintf(m->sp[0], sizeof(m->sp[0]), "%s", a);
    snprintf(m->sp[1], sizeof(m->sp[1]), "%s", b);
    snprintf(m->event, sizeof(m->event), "VCT 离线界面示例");
    m->logo[0] = -1;
    m->logo[1] = -1;
    m->have = true;
}

static void make_map(app_map_t *map, const char *name,
                     int a, int b, int icon) {
    memset(map, 0, sizeof(*map));
    snprintf(map->name, sizeof(map->name), "%s", name);
    map->score[0] = a;
    map->score[1] = b;
    map->rounds = a + b;
    map->win = a > b ? 0 : 1;
    map->icon = (int8_t)icon;
    map->have = true;
}

int app_preview_fill(app_match_t *matches, int match_cap,
                     app_match_detail_t *details, int detail_cap) {
    if (!matches || !details || match_cap < 5 || detail_cap < 2) return 0;
    make_match(&matches[0], 90001, "2026-09-27T16:00:00+08:00",
               APP_STATUS_DONE, "EDG", "PRX", 2, 1, 3);
    make_match(&matches[1], 90002, "2026-09-28T19:00:00+08:00",
               APP_STATUS_DONE, "G2", "SEN", 0, 2, 3);
    make_match(&matches[2], 90003, "2026-09-29T18:00:00+08:00",
               APP_STATUS_LIVE, "FNC", "DRX", 1, 0, 3);
    make_match(&matches[3], 90004, "2026-09-29T21:00:00+08:00",
               APP_STATUS_UPCOMING, "T1", "100T", 0, 0, 3);
    make_match(&matches[4], 90005, "2026-09-30T20:00:00+08:00",
               APP_STATUS_UPCOMING, "TL", "NRG", 0, 0, 5);

    memset(details, 0, sizeof(*details) * 2);
    details[0].match = matches[0];
    details[0].win_team = 0;
    details[0].map_count = 3;
    make_map(&details[0].maps[0], "亚海悬城", 13, 9, 0);
    make_map(&details[0].maps[1], "隐世修所", 10, 13, 1);
    make_map(&details[0].maps[2], "源工重镇", 13, 7, 2);

    details[1].match = matches[2];
    details[1].win_team = -1;
    details[1].map_count = 2;
    make_map(&details[1].maps[0], "亚海悬城", 13, 8, 0);
    make_map(&details[1].maps[1], "隐世修所", 6, 5, 1);
    return 5;
}
