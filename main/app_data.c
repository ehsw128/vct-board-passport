// main/app_data.c —— 数据层实现。
//
// 解析策略(见 app_json.h):赛季列表约 48KB、详情约 25KB,逐字节流过
// 扫描器,只保留展示字段;teamDesc/gameRoundList/gamePlayerStatisticsList
// 等大子树被静默跳过,内存占用与响应大小解耦。
#include "app_data.h"

#include <string.h>
#include <stdio.h>
#include "app_json.h"
#include "app_state.h"
#include "app_logos.h"
#include "app_map_icons.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "app_wifi.h"   // app_wifi_is_sta_connected()
#ifdef APP_OFFLINE_PREVIEW
#include "app_preview.h"
#endif

#define TAG "app_data"

// 数据源:腾讯 VCT 官网(vct.qq.com)前端使用的公开接口。
#define VAL_GAME_ID         1000074LL   // 2026 无畏契约全球冠军赛
#define URL_LIST_FMT \
    "https://val.native.game.qq.com/esports/v1/data/VAL_Match_%lld.json"
#define URL_DETAIL_FMT \
    "https://open.tjstats.com/val-auth-app/open/v1/statistics/match?matchId=%lld"
// open.tjstats.com 的公开前端 token(vct.qq.com 页面内嵌,非私密凭证)
#define TJSTATS_AUTH "7935be4c41d8760a28c05581a7b1f570"

#define HTTP_READ_BUF       1536
#define HTTP_MAX_RESPONSE   (256 * 1024)  // 响应有界保护
#define HTTP_MAX_TRANSIENT  2             // 连续读取超时后尽快失败并重试整次请求
#define WORKER_STACK        12288         // mbedtls 握手 + 解析临时区
#define DETAIL_CACHE        2             // 最近访问的详情环形缓存
#define EV_REFRESH          0x1

typedef struct {
    app_match_t raw[APP_RAW_MAX];
    int raw_count;
    app_data_status_t status;
    int64_t last_ok_ms;
    SemaphoreHandle_t lock;

    app_match_detail_t detail_cache[DETAIL_CACHE];
    int detail_next;                     // 环形替换指针

    volatile bool req_refresh;
    volatile int64_t req_detail_id;      // 0 = 无请求
    EventGroupHandle_t ev;               // bit0 = 立即唤醒

    // ---- 诊断字段(失败时屏幕可见) ----
    volatile int last_err;               // 最近一次 http_get_stream 的 esp_err_t
    volatile int last_tls_err;           // esp-tls 错误类型
    volatile int last_tls_code;          // mbedTLS 原始错误码
    volatile int last_tls_flags;         // 证书验证标志
    volatile int last_socket_errno;      // 建连时 socket errno
    volatile int last_bytes;             // 最近一次读到的字节数
    volatile int last_tmp_count;         // 解析出的临时条目数
    volatile bool last_doc_complete;     // 文档是否完整
} data_state_t;

static data_state_t s_data;

// ---------------------------------------------------------------- 解析:列表

typedef struct {
    app_match_t cur;                     // msg[] 当前元素累积
    int cur_arr_idx;                     // 当前 msg 元素下标;-1=尚无
    app_match_t *raw;
    int *count;
} list_ctx_t;

// msg[] 数组元素边界:回调携带元素下标,下标推进即提交上一条。
static void copy_utf8(char *dst, int dst_max, const char *src) {
    size_t n = strlen(src);
    if (n >= (size_t)dst_max) n = (size_t)dst_max - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void list_flush(list_ctx_t *c) {
    app_match_t *m = &c->cur;
    if (m->have && m->id != 0) {            // 不再要求队伍名:TBD 赛事也要展示
        // fallback:teamSpName 为空时用 teamName 顶上,保证列表不缺队伍列
        if (m->sp[0][0] == '\0' && m->team_name[0][0] != '\0') {
            copy_utf8(m->sp[0], APP_SP_MAX, m->team_name[0]);
        }
        if (m->sp[1][0] == '\0' && m->team_name[1][0] != '\0') {
            copy_utf8(m->sp[1], APP_SP_MAX, m->team_name[1]);
        }
        m->logo[0] = app_logos_find(m->sp[0]);
        m->logo[1] = app_logos_find(m->sp[1]);
        app_match_t *slot = NULL;
        for (int i = 0; i < *c->count; i++) {
            if (c->raw[i].id == m->id) { slot = &c->raw[i]; break; }
        }
        if (!slot) {
            if (*c->count < APP_RAW_MAX) {
                slot = &c->raw[(*c->count)++];
            } else {
                // 窗口已满:替换开赛最早的一条(保留最近赛事)
                slot = &c->raw[0];
                for (int i = 1; i < APP_RAW_MAX; i++) {
                    if (c->raw[i].start_ts < slot->start_ts) slot = &c->raw[i];
                }
                if (m->start_ts < slot->start_ts) slot = NULL;  // 更旧:丢弃
            }
        }
        if (slot) *slot = *m;
    }
    memset(m, 0, sizeof(*m));
}

static void list_on_value(void *user, app_json_ctx_t ctx, int idx,
                          const char *key, const char *str, double num) {
    list_ctx_t *c = (list_ctx_t *)user;
    if (ctx == CTX_MATCH) {
        if (idx != c->cur_arr_idx) {     // 进入下一条比赛
            list_flush(c);
            c->cur_arr_idx = idx;
        }
        if (strcmp(key, "bMatchId") == 0) {
            c->cur.have = true;
            c->cur.id = (int64_t)num;
        } else if (strcmp(key, "matchStatusId") == 0) {
            c->cur.status = (int)num;
        } else if (strcmp(key, "scoreA") == 0) {
            c->cur.score[0] = (int)num;
        } else if (strcmp(key, "scoreB") == 0) {
            c->cur.score[1] = (int)num;
        } else if (strcmp(key, "matchDate") == 0 && str) {
            c->cur.start_ts = app_state_parse_iso8601(str);
        } else if (strcmp(key, "matchFormat") == 0 && str) {
            c->cur.bo = app_state_parse_bo(str);
            if (c->cur.bo == 0) c->cur.bo = 1;
        } else if (strcmp(key, "matchType") == 0 && str) {
            copy_utf8(c->cur.stage, APP_STR_MAX, str);
        } else if (strcmp(key, "bMatchName") == 0 && str) {
            copy_utf8(c->cur.round_name, APP_STR_MAX, str);
        } else if (strcmp(key, "groupName") == 0 && str) {
            copy_utf8(c->cur.group, APP_STR_MAX, str);
        } else if (strcmp(key, "secondLevelGameName") == 0 && str) {
            copy_utf8(c->cur.event, APP_EVENT_MAX, str);
        }
    } else if (ctx == CTX_TEAM_A) {
        if (strcmp(key, "teamSpName") == 0 && str) {
            copy_utf8(c->cur.sp[0], APP_SP_MAX, str);
        } else if (strcmp(key, "teamName") == 0 && str) {
            copy_utf8(c->cur.team_name[0], APP_TEAM_MAX, str);
        }
    } else if (ctx == CTX_TEAM_B) {
        if (strcmp(key, "teamSpName") == 0 && str) {
            copy_utf8(c->cur.sp[1], APP_SP_MAX, str);
        } else if (strcmp(key, "teamName") == 0 && str) {
            copy_utf8(c->cur.team_name[1], APP_TEAM_MAX, str);
        }
    }
}

// ---------------------------------------------------------------- 解析:详情

typedef struct {
    app_match_detail_t *out;
    int cur_game;                        // CTX_GAME 当前图下标
} detail_ctx_t;

static void detail_on_value(void *user, app_json_ctx_t ctx, int idx,
                            const char *key, const char *str, double num) {
    detail_ctx_t *c = (detail_ctx_t *)user;
    app_match_detail_t *d = c->out;

    switch (ctx) {
    case CTX_STAT:                       // matchTeamStat[0/1]:系列赛总分
        if (idx < 0 || idx > 1) break;
        if (strcmp(key, "teamScore") == 0) {
            d->match.score[idx] = (int)num;
            d->match.have = true;
        } else if (strcmp(key, "matchWin") == 0 && num == 1.0) {
            d->win_team = idx;
        }
        break;

    case CTX_GAME:                       // gameStat[idx]:每一图
        if (idx < 0 || idx >= APP_MAP_MAX) break;
        c->cur_game = idx;
        if (strcmp(key, "mapNameCn") == 0 && str) {
            copy_utf8(d->maps[idx].name, APP_MAP_NAME_MAX, str);
            d->maps[idx].icon = -1;
            d->maps[idx].have = true;
        } else if (strcmp(key, "mapNameEn") == 0 && str) {
            d->maps[idx].icon = (int8_t)app_map_icons_find(str);
        } else if (strcmp(key, "totalRounds") == 0) {
            d->maps[idx].rounds = (int)num;
            d->maps[idx].have = true;
        }
        break;

    case CTX_GAME_TEAM_0:
    case CTX_GAME_TEAM_1: {
        if (c->cur_game < 0 || c->cur_game >= APP_MAP_MAX) break;
        int side = ctx == CTX_GAME_TEAM_0 ? 0 : 1;
        app_map_t *mp = &d->maps[c->cur_game];
        if (strcmp(key, "teamWinRound") == 0) {
            mp->score[side] = (int)num;
            mp->have = true;
        } else if (strcmp(key, "teamLoseRound") == 0) {
            if (mp->score[side ^ 1] == 0) mp->score[side ^ 1] = (int)num;
        } else if (strcmp(key, "gameWin") == 0 && num == 1.0) {
            mp->win = side;
            mp->have = true;
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------- HTTP

// 流式拉取:body 分块喂给扫描器,内存占用恒定。
static esp_err_t http_get_stream(const char *url, const char *auth,
                                 app_json_value_cb value_cb, void *parse_ctx) {
    s_data.last_tls_err = 0;
    s_data.last_tls_code = 0;
    s_data.last_tls_flags = 0;
    s_data.last_socket_errno = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = HTTP_READ_BUF,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    if (auth) {
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t err = ESP_FAIL;
    app_json_scanner_t sc;
    app_json_init(&sc, value_cb, parse_ctx);

    esp_err_t open_err = esp_http_client_open(client, 0);
    if (open_err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[HTTP_READ_BUF];
            int n;
            int64_t total = 0;
            bool over = false;
            bool doc_done = false;
            int transient_err = 0;
            // 容错读取:服务器为 chunked + Connection:close。
            // 旧版只要没超大小就把"读完"当成功,但 read 可能在只收到
            // 数组第一场时就结束(瞬时超时返回 -1 会直接跳出 while),
            // 于是快照只剩 1 场。这里以"根 JSON 已闭合"为成功标准,
            // 并对瞬时错误重试若干次。
            for (;;) {
                n = esp_http_client_read(client, buf, sizeof(buf));
                if (n > 0) {
                    total += n;
                    transient_err = 0;
                    if (total > HTTP_MAX_RESPONSE) {
                        over = true;
                        break;
                    }
                    app_json_feed(&sc, buf, n);
                    if (app_json_complete(&sc)) { doc_done = true; break; }
                } else if (n == 0) {
                    if (app_json_complete(&sc)) doc_done = true;
                    break;
                } else {
                    if (app_json_complete(&sc)) { doc_done = true; break; }
                    if (++transient_err > HTTP_MAX_TRANSIENT) break;
                }
            }
            if (doc_done) {
                err = ESP_OK;
            } else if (over) {
                ESP_LOGE(TAG, "响应超限(%lld 字节),中止", (long long)total);
                err = ESP_ERR_NO_MEM;
            } else {
                ESP_LOGE(TAG, "响应不完整(%lld 字节,瞬时错 %d)",
                         (long long)total, transient_err);
                err = ESP_ERR_INVALID_SIZE;
            }
            s_data.last_bytes = (int)total;
            s_data.last_doc_complete = doc_done;
        } else {
            ESP_LOGE(TAG, "HTTP %d %s", status, url);
            err = status > 0 ? (esp_err_t)status : ESP_FAIL;
            s_data.last_bytes = 0;
            s_data.last_doc_complete = false;
        }
    } else {
        int tls_code = 0;
        int tls_flags = 0;
        s_data.last_tls_err = (int)esp_http_client_get_and_clear_last_tls_error(
            client, &tls_code, &tls_flags);
        s_data.last_tls_code = tls_code;
        s_data.last_tls_flags = tls_flags;
        s_data.last_socket_errno = esp_http_client_get_errno(client);
        ESP_LOGE(TAG, "连接失败 %s: %s tls=%d mbed=%d flags=0x%x errno=%d",
                 url, esp_err_to_name(open_err), s_data.last_tls_err,
                 tls_code, tls_flags, s_data.last_socket_errno);
        err = open_err;
        s_data.last_bytes = 0;
        s_data.last_doc_complete = false;
    }
    s_data.last_err = (int)err;
    if (err == ESP_OK) {
        s_data.last_tls_err = 0;
        s_data.last_tls_code = 0;
        s_data.last_tls_flags = 0;
        s_data.last_socket_errno = 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------- worker

static int fetch_list_locked(void) {
    char url[128];
    snprintf(url, sizeof(url), URL_LIST_FMT, VAL_GAME_ID);

    list_ctx_t lctx;
    memset(&lctx, 0, sizeof(lctx));
    lctx.cur_arr_idx = -1;

    // 解析进静态临时区(worker 独占),成功后才替换快照(失败保留旧数据)。
    static app_match_t tmp[APP_RAW_MAX];
    int tmp_count = 0;
    lctx.raw = tmp;
    lctx.count = &tmp_count;

    esp_err_t e = http_get_stream(url, NULL, list_on_value, &lctx);
    list_flush(&lctx);                   // 提交最后一条
    s_data.last_tmp_count = tmp_count;

    if (e != ESP_OK || tmp_count == 0) {
        ESP_LOGE(TAG, "列表拉取失败 e=%s n=%d", esp_err_to_name(e), tmp_count);
        if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_data.status != APP_DATA_OK) s_data.status = APP_DATA_ERR;
            xSemaphoreGive(s_data.lock);
        }
        return -1;
    }

    if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(s_data.raw, tmp, sizeof(tmp));
        s_data.raw_count = tmp_count;
        s_data.status = APP_DATA_OK;
        s_data.last_ok_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_data.lock);
        ESP_LOGI(TAG, "列表更新:%d 场", tmp_count);
        return tmp_count;
    }
    return -1;
}

static void fetch_detail_locked(int64_t match_id) {
    // 命中缓存直接返回
    for (int i = 0; i < DETAIL_CACHE; i++) {
        if (s_data.detail_cache[i].match.id == match_id &&
            s_data.detail_cache[i].map_count > 0) {
            return;
        }
    }
    char url[128];
    snprintf(url, sizeof(url), URL_DETAIL_FMT, (long long)match_id);

    static app_match_detail_t tmp;       // worker 独占的解析临时区
    memset(&tmp, 0, sizeof(tmp));
    detail_ctx_t dctx = { .out = &tmp, .cur_game = -1 };

    esp_err_t e = http_get_stream(url, TJSTATS_AUTH, detail_on_value, &dctx);

    int map_count = 0;
    for (int i = 0; i < APP_MAP_MAX; i++) {
        if (tmp.maps[i].have) map_count = i + 1;
    }
    if (e != ESP_OK || map_count == 0) {
        ESP_LOGE(TAG, "详情拉取失败 id=%lld e=%s maps=%d",
                 (long long)match_id, esp_err_to_name(e), map_count);
        return;
    }
    tmp.map_count = map_count;

    if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        app_match_detail_t *slot = &s_data.detail_cache[s_data.detail_next];
        s_data.detail_next = (s_data.detail_next + 1) % DETAIL_CACHE;
        *slot = tmp;
        // 用列表快照补齐详情条目的展示字段(队名/轮次/队徽等)
        bool found = false;
        for (int i = 0; i < s_data.raw_count; i++) {
            if (s_data.raw[i].id == match_id) {
                slot->match = s_data.raw[i];
                found = true;
                break;
            }
        }
        if (!found) {
            // 快照缺失:logo 不能残留 0(那是有效下标),显式置为未收录
            slot->match.logo[0] = -1;
            slot->match.logo[1] = -1;
        }
        slot->match.id = match_id;
        xSemaphoreGive(s_data.lock);
        ESP_LOGI(TAG, "详情更新 id=%lld 图数=%d", (long long)match_id, map_count);
    }
}

static void worker(void *arg) {
    EventGroupHandle_t ev = (EventGroupHandle_t)arg;
    int failures = 0;
    int64_t time_wait_us = 0;
    for (;;) {
        bool detail_pending = s_data.req_detail_id != 0;

        if (app_wifi_can_fetch()) {
            // TLS 证书校验依赖正确时间。首次联网先等 SNTP，避免刚拿到 IP
            // 就用 1970 年时间请求 HTTPS 并进入错误状态。
            if (!app_wifi_sntp_synced()) {
                if (!time_wait_us) time_wait_us = esp_timer_get_time();
                if (s_data.raw_count == 0) {
                    if (esp_timer_get_time() - time_wait_us > 30000000) {
                        s_data.status = APP_DATA_ERR;
                        s_data.last_err = -4; // SNTP 未成功,供屏幕诊断
                    } else {
                        s_data.status = APP_DATA_FETCHING;
                    }
                }
                if (ev) xEventGroupWaitBits(ev, EV_REFRESH, pdTRUE, pdFALSE,
                                             pdMS_TO_TICKS(2000));
                else vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            time_wait_us = 0;
            int live = 0;
            bool requested = s_data.req_refresh;
            s_data.req_refresh = false;
            if (s_data.status == APP_DATA_IDLE ||
                s_data.status == APP_DATA_ERR ||
                s_data.status == APP_DATA_FETCHING || detail_pending || requested) {
                if (s_data.raw_count == 0) s_data.status = APP_DATA_FETCHING;
                live = fetch_list_locked();
                failures = live < 0 ? (failures < 3 ? failures + 1 : 3) : 0;
                if (detail_pending) {
                    int64_t id = s_data.req_detail_id;
                    s_data.req_detail_id = 0;
                    fetch_detail_locked(id);
                } else if (live > 0) {
                    // 有进行中比赛时预热第一场直播的详情(进入详情页零等待)
                    if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(500)) ==
                        pdTRUE) {
                        for (int i = 0; i < s_data.raw_count; i++) {
                            if (s_data.raw[i].status == APP_STATUS_LIVE) {
                                int64_t id = s_data.raw[i].id;
                                xSemaphoreGive(s_data.lock);
                                fetch_detail_locked(id);
                                goto scheduled;
                            }
                        }
                        xSemaphoreGive(s_data.lock);
                    }
                }
            }
scheduled:;
            // 首次失败后逐级退避重试，成功后恢复正常刷新节奏。
            int wait_s = failures ? (failures == 1 ? 5 : failures == 2 ? 15 : 30)
                                  : (live > 0 ? 30 : 60);
            TickType_t wait = pdMS_TO_TICKS(wait_s * 1000);
            EventBits_t bits = ev ? xEventGroupWaitBits(ev, EV_REFRESH, pdTRUE,
                                                        pdFALSE, wait)
                                  : 0;
            (void)bits;                  // 唤醒后回到循环顶立即刷新
        } else {
            // 未联网:每 2s 检查一次连接状态
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}

// ---------------------------------------------------------------- 公共 API

void app_data_init(void) {
    memset(&s_data, 0, sizeof(s_data));
    s_data.lock = xSemaphoreCreateMutex();
    s_data.status = APP_DATA_IDLE;
    s_data.ev = xEventGroupCreate();
#ifdef APP_OFFLINE_PREVIEW
    s_data.raw_count = app_preview_fill(s_data.raw, APP_RAW_MAX,
                                        s_data.detail_cache, DETAIL_CACHE);
    for (int i = 0; i < s_data.raw_count; i++) {
        s_data.raw[i].logo[0] = app_logos_find(s_data.raw[i].sp[0]);
        s_data.raw[i].logo[1] = app_logos_find(s_data.raw[i].sp[1]);
    }
    for (int i = 0; i < DETAIL_CACHE; i++) {
        for (int j = 0; j < s_data.raw_count; j++) {
            if (s_data.detail_cache[i].match.id == s_data.raw[j].id) {
                s_data.detail_cache[i].match = s_data.raw[j];
                break;
            }
        }
    }
    s_data.status = APP_DATA_OK;
    s_data.last_ok_ms = 1;
    return;
#endif
    if (xTaskCreate(worker, "app_data", WORKER_STACK, s_data.ev, 4, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "worker 任务创建失败");
    }
}

int app_data_get_matches(app_match_t *out, int max) {
    int n = 0;
    if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < s_data.raw_count && n < max; i++) {
            out[n++] = s_data.raw[i];
        }
        xSemaphoreGive(s_data.lock);
    }
    return n;
}

bool app_data_get_detail(int64_t id, app_match_detail_t *out) {
    bool found = false;
    if (xSemaphoreTake(s_data.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < DETAIL_CACHE; i++) {
            if (s_data.detail_cache[i].match.id == id &&
                s_data.detail_cache[i].map_count > 0) {
                *out = s_data.detail_cache[i];
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_data.lock);
    }
    return found;
}

void app_data_request_refresh(void) {
    s_data.req_refresh = true;
    if (s_data.ev) xEventGroupSetBits(s_data.ev, EV_REFRESH);
}

void app_data_request_detail(int64_t match_id) {
    s_data.req_detail_id = match_id;
    if (s_data.ev) xEventGroupSetBits(s_data.ev, EV_REFRESH);
}

app_data_status_t app_data_status(void) {
    return s_data.status;
}

int64_t app_data_last_ok_ms(void) {
    return s_data.last_ok_ms;
}

bool app_data_is_preview(void) {
#ifdef APP_OFFLINE_PREVIEW
    return true;
#else
    return false;
#endif
}

int  app_data_diag_err(void)      { return s_data.last_err; }
int  app_data_diag_tls_err(void)  { return s_data.last_tls_err; }
int  app_data_diag_tls_code(void) { return s_data.last_tls_code; }
int  app_data_diag_tls_flags(void) { return s_data.last_tls_flags; }
int  app_data_diag_errno(void)    { return s_data.last_socket_errno; }
int  app_data_diag_bytes(void)    { return s_data.last_bytes; }
int  app_data_diag_count(void)    { return s_data.last_tmp_count; }
bool app_data_diag_complete(void) { return s_data.last_doc_complete; }
