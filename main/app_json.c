// main/app_json.c —— 流式 JSON 扫描器实现。
// 逐字节状态机,token 可跨 HTTP chunk 续接;嵌套深度固定(6 层栈),
// 无动态内存。大数组(gameRoundList/matchPlayerStat)与无关对象(live 等)
// 进栈时标记 muted,其内部所有值不再回调,扫描开销降为纯字节遍历。
#include "app_json.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

// 进入即静默的子树:到达 key 命中后,整棵子树的值都不回调。
// gameRoundList/matchPlayerStat 是逐回合/逐选手的大数组,必须跳过。
static const char *const MUTE_KEYS[] = {
    "gameRoundList", "matchPlayerStat", "gamePlayerStatisticsList",
    "live", "progress", "specialCover", "consoleVideo", "cloudStream",
};
#define MUTE_KEY_COUNT (sizeof(MUTE_KEYS) / sizeof(MUTE_KEYS[0]))

typedef enum {
    ST_ROOT = 0,   // 栈空,等待首个 '{' 或 '['
    ST_KEY,        // 对象内,等待 "key"
    ST_COLON,      // 等待 ':'
    ST_VALUE,      // 等待值的首字符
    ST_NUM,        // 数字值收集中
    ST_LIT,        // true/false/null 字面量收集中
    ST_NEXT,       // 值结束,等待 ',' '}' ']'
    ST_SKIP_U,     // \uXXXX 转义的 4 位 hex 丢弃中
} json_state_t;

static bool level_muted(const app_json_scanner_t *sc) {
    for (int i = 0; i < sc->depth; i++) {
        if (sc->st[i].muted) return true;
    }
    return false;
}

static bool key_is_mute(const char *key) {
    for (size_t i = 0; i < MUTE_KEY_COUNT; i++) {
        if (strcmp(key, MUTE_KEYS[i]) == 0) return true;
    }
    return false;
}

// 按到达路径推导对象层的回调上下文与下标。
static void derive_ctx(app_json_scanner_t *sc, app_json_ctx_t *ctx, int *idx) {
    const app_json_level_t *parent = sc->depth > 0 ? &sc->st[sc->depth - 1] : NULL;

    if (parent && parent->is_array) {
        *idx = parent->idx;
        if (strcmp(parent->key, "msg") == 0) { *ctx = CTX_MATCH; return; }
        if (strcmp(parent->key, "gameStat") == 0) { *ctx = CTX_GAME; return; }
        if (strcmp(parent->key, "matchTeamStat") == 0) { *ctx = CTX_STAT; return; }
        if (strcmp(parent->key, "gameTeamStatisticsList") == 0) {
            // 数组元素对象的到达 key 在 push 时已被清空,无法再查祖父层,
            // 但该数组名在本接口中唯一,直接按数组名推导
            *ctx = parent->idx == 0 ? CTX_GAME_TEAM_0 : CTX_GAME_TEAM_1;
            return;
        }
        *ctx = CTX_OTHER;
        return;
    }

    *idx = 0;
    if (parent) *idx = parent->idx;   // 对象层继承父层条目下标(teamA 随所在 msg[i])
    if (strcmp(sc->key, "teamA") == 0) { *ctx = CTX_TEAM_A; return; }
    if (strcmp(sc->key, "teamB") == 0) { *ctx = CTX_TEAM_B; return; }
    *ctx = CTX_OTHER;
}

static void copy_str(char *dst, int dst_max, const char *src) {
    size_t n = strlen(src);
    if (n > (size_t)dst_max) n = (size_t)dst_max;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void push_level(app_json_scanner_t *sc, bool is_array) {
    if (sc->overflow) return;
    if (sc->depth >= APP_JSON_MAX_DEPTH) {
        sc->overflow = true;           // 结构超出预期:丢弃该子树
        return;
    }
    app_json_level_t *L = &sc->st[sc->depth];
    copy_str(L->key, (int)sizeof(L->key) - 1, sc->key);
    L->is_array = is_array;
    L->idx = 0;
    derive_ctx(sc, &L->ctx, &L->idx);
    // 静默规则:继承父层静默;命中静默名单;非白名单的命名对象
    // (live/杂项,ctx==OTHER 且有到达 key)也整棵跳过。根对象与 data
    // 容器到达 key 为空/"data",保持活跃。
    L->muted = level_muted(sc) || key_is_mute(sc->key) ||
               (!is_array && L->ctx == CTX_OTHER && sc->key[0] != '\0' &&
                strcmp(sc->key, "data") != 0);
    sc->depth++;
    if (sc->depth == 1) sc->root_seen = true;   // 首个容器=根
    sc->key[0] = '\0';
    sc->state = is_array ? ST_VALUE : ST_KEY;
}

static void emit_value(app_json_scanner_t *sc, const char *str, double num) {
    if (sc->depth == 0 || sc->overflow || level_muted(sc)) return;
    const app_json_level_t *top = &sc->st[sc->depth - 1];
    if (top->is_array) return;                 // 裸数组标量:不在白名单
    sc->cb(sc->user, top->ctx, top->idx, sc->key, str, num);
}

static void str_end(app_json_scanner_t *sc) {
    sc->in_str = false;
    sc->val[sc->val_len] = '\0';
    if (sc->cap_kind == 1) {
        copy_str(sc->key, APP_JSON_KEY_MAX, sc->val);
        sc->state = ST_COLON;
    } else if (sc->cap_kind == 2) {
        emit_value(sc, sc->val, 0.0);
        sc->state = ST_NEXT;
    }
    sc->cap_kind = 0;
}

static void handle_escape(app_json_scanner_t *sc, char c) {
    char out;
    switch (c) {
    case '"': out = '"'; break;
    case '\\': out = '\\'; break;
    case '/': out = '/'; break;
    case 'n': case 'r': case 't': case 'b': case 'f': out = ' '; break;
    case 'u':  // \uXXXX:目标接口值均为原生 UTF-8,丢弃该转义序列
        sc->state = ST_SKIP_U;
        sc->u_skip = 0;
        return;
    default: out = c; break;
    }
    if (sc->cap_kind && sc->val_len < APP_JSON_VAL_MAX) {
        sc->val[sc->val_len++] = out;
    }
}

void app_json_init(app_json_scanner_t *sc, app_json_value_cb cb, void *user) {
    memset(sc, 0, sizeof(*sc));
    sc->cb = cb;
    sc->user = user;
    sc->state = ST_ROOT;
}

bool app_json_complete(const app_json_scanner_t *sc) {
    return sc->root_seen && sc->depth == 0;
}

static void feed_byte(app_json_scanner_t *sc, char c);

// 数字/字面量结束后的分隔符本身可能还是结构符,重放一次。
static void value_done_replay(app_json_scanner_t *sc, char c) {
    feed_byte(sc, c);
}

// 闭合当前层级:处理 } / ]。与 ST_NEXT 的闭合逻辑一致。
static void close_level(app_json_scanner_t *sc) {
    if (sc->depth > 0) sc->depth--;
    if (sc->depth == 0) sc->overflow = false;
    sc->state = sc->depth == 0 ? ST_ROOT : ST_NEXT;
}

static void feed_byte(app_json_scanner_t *sc, char c) {
    if (sc->state == ST_SKIP_U) {
        const char *hex = strchr("0123456789abcdefABCDEF", c);
        if (hex && c != '\0') {
            if (++sc->u_skip >= 4) sc->state = ST_NEXT;
        } else {
            sc->state = ST_NEXT;
            value_done_replay(sc, c);
        }
        return;
    }

    if (sc->in_str) {
        if (sc->esc) {
            sc->esc = false;
            handle_escape(sc, c);
            return;
        }
        if (c == '\\') { sc->esc = true; return; }
        if (c == '"') { str_end(sc); return; }
        if (sc->cap_kind && sc->val_len < APP_JSON_VAL_MAX) {
            sc->val[sc->val_len++] = c;
        }
        return;
    }

    switch (sc->state) {
    case ST_ROOT:
        if (c == '{') push_level(sc, false);
        else if (c == '[') push_level(sc, true);
        break;

    case ST_KEY:
        if (c == '"') {
            sc->in_str = true;
            sc->cap_kind = 1;
            sc->val_len = 0;
        } else if (c == '}') {
            // 空对象 {}:还没有任何 key 就闭合
            close_level(sc);
        }
        break;

    case ST_COLON:
        if (c == ':') sc->state = ST_VALUE;
        break;

    case ST_VALUE:
        if (c == '"') {
            sc->in_str = true;
            sc->cap_kind = 2;
            sc->val_len = 0;
        } else if (c == '{') {
            push_level(sc, false);
        } else if (c == '[') {
            push_level(sc, true);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            sc->cap_kind = 3;
            sc->val_len = 0;
            sc->val[sc->val_len++] = c;
            sc->state = ST_NUM;
        } else if (c == 't' || c == 'f' || c == 'n') {
            sc->cap_kind = 3;              // 字面量与数字共用 val 缓冲
            sc->val_len = 0;
            sc->val[sc->val_len++] = c;
            sc->state = ST_LIT;
        } else if (c == ']' || c == '}') {
            // 空数组 [] / 空对象 {}:容器刚打开还没有值就闭合
            close_level(sc);
        }
        break;

    case ST_NUM:
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') {
            if (sc->val_len < APP_JSON_VAL_MAX) sc->val[sc->val_len++] = c;
        } else {
            sc->val[sc->val_len] = '\0';
            emit_value(sc, NULL, strtod(sc->val, NULL));
            sc->cap_kind = 0;
            sc->state = ST_NEXT;
            value_done_replay(sc, c);
        }
        break;

    case ST_LIT:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (sc->val_len < APP_JSON_VAL_MAX) sc->val[sc->val_len++] = c;
        } else {
            sc->val[sc->val_len] = '\0';
            // 布尔值以 1/0 回调(matchWin/gameWin);null 不回调。
            if (strcmp(sc->val, "true") == 0) emit_value(sc, NULL, 1.0);
            else if (strcmp(sc->val, "false") == 0) emit_value(sc, NULL, 0.0);
            sc->cap_kind = 0;
            sc->state = ST_NEXT;
            value_done_replay(sc, c);
        }
        break;

    case ST_NEXT:
        if (c == ',') {
            app_json_level_t *top = sc->depth > 0 ? &sc->st[sc->depth - 1] : NULL;
            if (top && top->is_array) {
                top->idx++;
                sc->state = ST_VALUE;      // 数组下一个元素
            } else {
                sc->state = ST_KEY;
            }
            sc->key[0] = '\0';
        } else if (c == '}' || c == ']') {
            if (sc->depth > 0) sc->depth--;
            if (sc->depth == 0) sc->overflow = false;
            sc->state = sc->depth == 0 ? ST_ROOT : ST_NEXT;
        }
        break;

    default:
        break;
    }
}

void app_json_feed(app_json_scanner_t *sc, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        feed_byte(sc, data[i]);
    }
}
