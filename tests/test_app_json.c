// tests/test_app_json.c —— 流式 JSON 扫描器 host 测试。
// 覆盖:字段提取、跨 chunk 断词、muted 子树跳过、布尔值、msg 数组边界、
//       teamA/teamB 区分、详情接口路径推导。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_json.h"

typedef struct {
    // 列表
    int match_count;
    int64_t ids[4];
    char sp_a[4][10];
    char sp_b[4][10];
    int score_a;
    int score_b;
    char date[40];
    int bo;
    char stage[24];
    char round_name[24];
    char group[24];
    int status;
    // 详情
    int stat_score[2];
    int win_team;
    char map0_name[24];
    int map0_rounds;
    int map0_score[2];
    int map0_win;
    // 违规回调计数(muted 子树不应产生)
    int muted_hits;
} capture_t;

static void on_value(void *user, app_json_ctx_t ctx, int idx,
                     const char *key, const char *str, double num) {
    capture_t *c = (capture_t *)user;
    if (ctx == CTX_MATCH) {
        if (strcmp(key, "bMatchId") == 0) {
            if (idx < 4) c->ids[idx] = (int64_t)num;
            if (idx + 1 > c->match_count) c->match_count = idx + 1;
        } else if (idx == 0) {
            // 展示字段只取第一条(第二条会覆盖,单值捕获按条目 0 断言)
            if (strcmp(key, "matchStatusId") == 0) {
                c->status = (int)num;
            } else if (strcmp(key, "scoreA") == 0) {
                c->score_a = (int)num;
            } else if (strcmp(key, "scoreB") == 0) {
                c->score_b = (int)num;
            } else if (strcmp(key, "matchDate") == 0 && str) {
                snprintf(c->date, sizeof(c->date), "%s", str);
            } else if (strcmp(key, "matchFormat") == 0 && str) {
                c->bo = str[2] - '0';
            } else if (strcmp(key, "matchType") == 0 && str) {
                snprintf(c->stage, sizeof(c->stage), "%s", str);
            } else if (strcmp(key, "bMatchName") == 0 && str) {
                snprintf(c->round_name, sizeof(c->round_name), "%s", str);
            } else if (strcmp(key, "groupName") == 0 && str) {
                snprintf(c->group, sizeof(c->group), "%s", str);
            }
        }
    } else if (ctx == CTX_TEAM_A) {
        if (strcmp(key, "teamSpName") == 0 && str && idx < 4) {
            snprintf(c->sp_a[idx], sizeof(c->sp_a[0]), "%s", str);
        }
    } else if (ctx == CTX_TEAM_B) {
        if (strcmp(key, "teamSpName") == 0 && str && idx < 4) {
            snprintf(c->sp_b[idx], sizeof(c->sp_b[0]), "%s", str);
        }
    } else if (ctx == CTX_STAT) {
        if (strcmp(key, "teamScore") == 0) c->stat_score[idx] = (int)num;
        else if (strcmp(key, "matchWin") == 0 && num == 1.0) c->win_team = idx;
    } else if (ctx == CTX_GAME) {
        if (idx == 0) {
            if (strcmp(key, "mapNameCn") == 0 && str) {
                snprintf(c->map0_name, sizeof(c->map0_name), "%s", str);
            } else if (strcmp(key, "totalRounds") == 0) {
                c->map0_rounds = (int)num;
            }
        }
    } else if (ctx == CTX_GAME_TEAM_0) {
        if (strcmp(key, "teamWinRound") == 0) c->map0_score[0] = (int)num;
        else if (strcmp(key, "teamLoseRound") == 0) c->map0_score[1] = (int)num;
        else if (strcmp(key, "gameWin") == 0 && num == 1.0) c->map0_win = 0;
    } else if (ctx == CTX_GAME_TEAM_1) {
        if (strcmp(key, "gameWin") == 0 && num == 1.0) c->map0_win = 1;
    }
}

// muted 子树内的字段一旦被回调即视为违规
static void on_value_strict(void *user, app_json_ctx_t ctx, int idx,
                            const char *key, const char *str, double num) {
    capture_t *c = (capture_t *)user;
    (void)ctx; (void)idx; (void)str; (void)num;
    if (strcmp(key, "roundType") == 0 || strcmp(key, "winningTeam") == 0 ||
        strcmp(key, "anchorId") == 0) {
        c->muted_hits++;
    }
}

static void feed_all(app_json_scanner_t *sc, const char *json, int chunk) {
    int len = (int)strlen(json);
    for (int i = 0; i < len; i += chunk) {
        int n = len - i < chunk ? len - i : chunk;
        app_json_feed(sc, json + i, n);
    }
}

static const char LIST_JSON[] =
    "{\"status\":0,\"msg\":["
    "{\"matchFormatId\":2,\"progressId\":0,\"scoreA\":2,\"consoleVideo\":\"\","
    "\"bMatchId\":1002725,\"scoreB\":0,\"groupName\":\"A组\","
    "\"secondLevelGameName\":\"2026无畏契约全球冠军赛\",\"matchStatusId\":3,"
    "\"bMatchName\":\"A组第一轮\",\"matchFormat\":\"BO3\",\"matchType\":\"小组赛\","
    "\"matchDate\":\"2026-09-27T17:00:00+08:00\","
    "\"live\":{\"anchorId\":2023010101,\"focusMatch\":\"1\"},"
    "\"teamA\":{\"teamId\":122,\"teamName\":\"100 Thieves\","
    "\"teamSpName\":\"100T\",\"division\":\"\",\"teamDesc\":\"很长很长的介绍..."
    "\\u5e26\\u8f6c\\u4e49\","
    "\"teamLightLogo\":\"https://esports.val.qq.com/x.png\"},"
    "\"teamB\":{\"teamShortName\":\"T1\",\"division\":\"太平洋国际联赛\","
    "\"teamDesc\":\"另一段介绍\",\"teamId\":60,\"teamName\":\"T1\","
    "\"teamSpName\":\"T1\"}},"
    "{\"bMatchId\":1002726,\"matchStatusId\":1,\"scoreA\":0,\"scoreB\":0,"
    "\"matchFormat\":\"BO5\",\"teamA\":{\"teamSpName\":\"G2\"},"
    "\"teamB\":{\"teamSpName\":\"PRX\"}}"
    "],\"lastUpdateTime\":\"2026-09-28T12:12:00.752Z\"}";

static const char DETAIL_JSON[] =
    "{\"code\":0,\"data\":{"
    "\"matchTeamStat\":["
    "{\"teamInfo\":{\"teamName\":\"100T\"},\"teamScore\":2,\"matchWin\":true,"
    "\"matchStatus\":2,\"matchPlayerStat\":[{\"playerId\":1,\"acs\":289}]},"
    "{\"teamInfo\":{\"teamName\":\"T1\"},\"teamScore\":0,\"matchWin\":false,"
    "\"matchStatus\":2,\"matchPlayerStat\":[{\"playerId\":2,\"acs\":190}]}],"
    "\"gameStat\":["
    "{\"matchId\":1002725,\"bo\":1,\"totalRounds\":21,\"mapId\":1,"
    "\"mapNameCn\":\"天枢云阙\",\"mapNameEn\":\"Abyss\",\"gameStatus\":2,"
    "\"gameRoundList\":[{\"roundType\":1,\"winningTeam\":0}],"
    "\"gameTeamStatisticsList\":["
    "{\"teamId\":122,\"teamName\":\"100T\",\"gameWin\":true,"
    "\"teamWinRound\":13,\"teamLoseRound\":8,"
    "\"gamePlayerStatisticsList\":[{\"playerId\":9}]},"
    "{\"teamId\":60,\"teamName\":\"T1\",\"gameWin\":false,"
    "\"teamWinRound\":8,\"teamLoseRound\":13,"
    "\"gamePlayerStatisticsList\":[{\"playerId\":8}]}]}"
    "]},\"success\":true}";

int main(void) {
    // ---- 列表解析 ----
    capture_t cap;
    memset(&cap, 0, sizeof(cap));
    app_json_scanner_t sc;
    app_json_init(&sc, on_value, &cap);
    feed_all(&sc, LIST_JSON, 1);          // 逐字节
    assert(cap.match_count == 2);
    assert(cap.ids[0] == 1002725);
    assert(cap.ids[1] == 1002726);
    assert(strcmp(cap.sp_a[0], "100T") == 0);
    assert(strcmp(cap.sp_b[0], "T1") == 0);
    assert(strcmp(cap.sp_a[1], "G2") == 0);
    assert(strcmp(cap.sp_b[1], "PRX") == 0);
    assert(cap.score_a == 2 && cap.score_b == 0);
    assert(cap.status == 3);
    assert(cap.bo == 3);
    assert(strcmp(cap.date, "2026-09-27T17:00:00+08:00") == 0);
    assert(strcmp(cap.stage, "小组赛") == 0);
    assert(strcmp(cap.round_name, "A组第一轮") == 0);
    assert(strcmp(cap.group, "A组") == 0);

    // ---- 跨 chunk(7 字节块,token 被切碎) ----
    memset(&cap, 0, sizeof(cap));
    app_json_init(&sc, on_value, &cap);
    feed_all(&sc, LIST_JSON, 7);
    assert(cap.match_count == 2);
    assert(cap.ids[0] == 1002725);
    assert(strcmp(cap.sp_a[0], "100T") == 0);
    assert(strcmp(cap.date, "2026-09-27T17:00:00+08:00") == 0);

    // ---- muted 子树(live/gameRoundList/matchPlayerStat/gamePlayerStatisticsList) ----
    memset(&cap, 0, sizeof(cap));
    app_json_init(&sc, on_value_strict, &cap);
    feed_all(&sc, LIST_JSON, 64);
    feed_all(&sc, DETAIL_JSON, 64);
    assert(cap.muted_hits == 0);

    // ---- 详情解析 ----
    memset(&cap, 0, sizeof(cap));
    app_json_init(&sc, on_value, &cap);
    feed_all(&sc, DETAIL_JSON, 32);
    assert(cap.stat_score[0] == 2 && cap.stat_score[1] == 0);
    assert(cap.win_team == 0);
    assert(strcmp(cap.map0_name, "天枢云阙") == 0);
    assert(cap.map0_rounds == 21);
    assert(cap.map0_score[0] == 13 && cap.map0_score[1] == 8);
    assert(cap.map0_win == 0);

    printf("test_app_json: PASS\n");
    return 0;
}
