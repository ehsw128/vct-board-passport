// main/app_model.h —— 无畏契约赛事看板的数据模型。
// 所有结构都是"紧凑快照":只保留看板展示需要的字段,
// 原始 JSON(单场约 500B + 详情 25KB)解析后立即收敛到这里,避免占用堆。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 当前赛事列表可超过 30 场；保留完整近期赛程，避免较晚的未开赛场次
// 把已结束场次从日期视图中挤掉。
#define APP_RAW_MAX            64
#define APP_VIEW_MAX           APP_RAW_MAX

#define APP_SP_MAX             10    // teamSpName,如 "100T"
#define APP_TEAM_MAX           24    // teamName,如 "100 Thieves"
#define APP_STR_MAX            24    // bMatchName/groupName/matchType
#define APP_DATE_MAX           40    // matchDate ISO8601 原串(含时区)
#define APP_EVENT_MAX          36    // secondLevelGameName,如 "2026无畏契约全球冠军赛"
#define APP_MAP_MAX            5     // BO5 最多 5 图
#define APP_MAP_NAME_MAX       24    // mapNameCn,如 "天枢云阙"

// API matchStatusId 语义(VAL_Match_*.json)
#define APP_STATUS_UPCOMING    1
#define APP_STATUS_LIVE        2
#define APP_STATUS_DONE        3

typedef struct {
    int64_t id;                      // bMatchId,详情接口入参
    int status;                      // APP_STATUS_*
    int score[2];                    // 总比分 A/B(进行中/已结束有效)
    int64_t start_ts;                // 开赛时间 epoch 秒(UTC,由 matchDate 解析)
    int bo;                          // 3/5;"BO1"或未知=1,缺省 0
    char sp[2][APP_SP_MAX];          // 双方简称
    char team_name[2][APP_TEAM_MAX]; // 双方全名(详情页)
    int8_t logo[2];                  // 内置队徽下标(app_logos_find);-1=未收录
    char group[APP_STR_MAX];         // 组别,如 "A组"
    char round_name[APP_STR_MAX];    // 轮次,如 "A组胜者组决赛"
    char stage[APP_STR_MAX];         // 赛段,如 "小组赛"
    char event[APP_EVENT_MAX];       // 赛事全名,如 "2026无畏契约全球冠军赛"
    bool have;                       // 条目有效
} app_match_t;

typedef struct {
    char name[APP_MAP_NAME_MAX];     // 中文地图名
    int score[2];                    // 每图回合比分
    int win;                         // 胜方 0/1;-1 未开打
    int rounds;                      // 总回合数;0 未开打
    int8_t icon;                     // 地图图标下标(app_map_icons_find);-1=未收录
    bool have;
} app_map_t;

typedef struct {
    app_match_t match;               // 列表快照(从缓存复制)
    app_map_t maps[APP_MAP_MAX];
    int map_count;
    int win_team;                    // 系列赛胜方 0/1;-1 未定
} app_match_detail_t;

#ifdef __cplusplus
}
#endif
