// main/app_data.h —— 赛程/详情数据层:HTTP 拉取 + 流式解析 + worker 任务。
//
// 线程模型:
//   worker 任务(AppData)负责全部网络与解析,结果写入内部快照(互斥保护);
//   UI/主任务只读快照,永不阻塞在网络上。
// 刷新节奏:有进行中比赛 30s,否则 60s;也可立即请求刷新。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_DATA_IDLE = 0,       // 尚未拉取过
    APP_DATA_FETCHING,       // 正在请求
    APP_DATA_OK,             // 最近一次列表拉取成功
    APP_DATA_ERR,            // 最近一次失败(网络/解析/HTTP 状态)
} app_data_status_t;

void app_data_init(void);

// 请求立即刷新列表(异步,worker 处理)。
void app_data_request_refresh(void);

// 请求某场比赛的逐图详情(异步)。完成后 app_data_get_detail 可取。
void app_data_request_detail(int64_t match_id);

// 快照读取(线程安全,内部持锁,拷贝出栈)。
int app_data_get_matches(app_match_t *out, int max);
bool app_data_get_detail(int64_t id, app_match_detail_t *out);

app_data_status_t app_data_status(void);
int64_t app_data_last_ok_ms(void);   // 最近一次成功时间(monotonic ms);0=从未
bool app_data_is_preview(void);       // 离线模拟器构建使用内置示例数据

// 诊断:最近一次列表请求的 esp_err_t / 字节数 / 解析条目数 / 文档是否完整。
int  app_data_diag_err(void);
int  app_data_diag_tls_err(void);
int  app_data_diag_tls_code(void);
int  app_data_diag_tls_flags(void);
int  app_data_diag_errno(void);
int  app_data_diag_bytes(void);
int  app_data_diag_count(void);
bool app_data_diag_complete(void);

#ifdef __cplusplus
}
#endif
