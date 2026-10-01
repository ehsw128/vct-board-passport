// main/app_ui.h —— LVGL 界面(240×320 竖屏,中文)。
// 页面:启动状态 -> 赛程看板(列表) -> 逐图详情 -> 配网。
// 所有函数都必须在持有 bsp_lvgl_lock 的情况下调用(除 init 内部已持锁场景)。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_UI_KEY_UP = 0,
    APP_UI_KEY_DOWN,
    APP_UI_KEY_OK,
    APP_UI_KEY_OK_LONG,
} app_ui_key_t;

// 创建首屏。必须在 bsp_lvgl_lock 下调用一次。
void app_ui_init(void);

// 按键分发(在 bsp_lvgl_lock 下调用)。
void app_ui_key(app_ui_key_t key);

#ifdef __cplusplus
}
#endif
