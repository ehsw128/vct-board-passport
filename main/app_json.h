// main/app_json.h —— 面向腾讯 VCT 接口的流式 JSON 扫描器(可 host 测试)。
//
// 为什么不用 cJSON:赛季列表约 48KB、单场详情约 25KB,整包解析需要同量级
// 堆缓冲(历史教训见 AGENTS.md:4KB doc 解析 7KB payload 曾崩溃)。本扫描器
// 逐字节消费 HTTP chunk,只对白名单路径回调,大字段(gameRoundList/
// matchPlayerStat/teamDesc 等)以 muted 子树方式跳过,内存占用恒定。
//
// 回调上下文 ctx 由扫描器按"到达路径"推导,见 app_json_ctx_t。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 值所在容器,按到达路径判定:
//   msg[] 元素对象                 -> CTX_MATCH        (赛程条目)
//   teamA/teamB 对象               -> CTX_TEAM_A/B     (队伍字段)
//   gameStat[] 元素                -> CTX_GAME         (每一图,idx=图序)
//   gameStat[].gameTeamStatisticsList[i]
//                                  -> CTX_GAME_TEAM_0/1(每图双方回合数据)
//   matchTeamStat[] 元素           -> CTX_STAT         (idx=0/1/2,取 2 为系列赛总分)
//   其余(live/progress/标量杂项)   -> CTX_OTHER
typedef enum {
    CTX_OTHER = 0,
    CTX_MATCH,
    CTX_TEAM_A,
    CTX_TEAM_B,
    CTX_GAME,
    CTX_GAME_TEAM_0,
    CTX_GAME_TEAM_1,
    CTX_STAT,
} app_json_ctx_t;

typedef void (*app_json_value_cb)(void *user, app_json_ctx_t ctx, int idx,
                                  const char *key, const char *str, double num);

// 完整类型公开:调用方(数据层/测试)在栈上静态实例化,无动态内存。
#define APP_JSON_MAX_DEPTH   6
#define APP_JSON_KEY_MAX     30   // key 缓冲(不含 NUL)
#define APP_JSON_VAL_MAX     63   // 值缓冲(不含 NUL);matchDate 25B、队名远小于此

typedef struct {
    char key[24];            // 到达该层的 key:须容纳 "gameTeamStatisticsList"(22B)
    int idx;                 // 数组层=当前元素下标;对象层=从父数组复制的下标
    bool is_array;
    bool muted;
    app_json_ctx_t ctx;      // 该层值的回调上下文(由到达路径推导)
} app_json_level_t;

typedef struct {
    app_json_level_t st[APP_JSON_MAX_DEPTH];
    int depth;
    bool root_seen;          // 已遇到根容器:depth 回 0 即文档完整
    bool overflow;           // 嵌套超过栈深:丢弃后续所有子树
    int u_skip;              // \uXXXX 已丢弃的 hex 位数
    int state;               // json_state_t(实现私有,公开仅为栈分配)
    char key[APP_JSON_KEY_MAX + 1];
    char val[APP_JSON_VAL_MAX + 1];
    int val_len;
    bool in_str;
    bool esc;
    int cap_kind;            // 0=丢弃 1=key 2=值字符串 3=数字
    app_json_value_cb cb;
    void *user;
} app_json_scanner_t;

void app_json_init(app_json_scanner_t *sc, app_json_value_cb cb, void *user);
// 喂入任意长度分块,块边界可落在任何字节上(token 自动跨块续接)。
void app_json_feed(app_json_scanner_t *sc, const char *data, int len);
// 根容器已闭合=完整文档(即使 socket 之后报错也可接受)。
bool app_json_complete(const app_json_scanner_t *sc);

#ifdef __cplusplus
}
#endif
