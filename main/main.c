// main/main.c —— 无畏契约赛事看板应用入口。
//
// 结构:BSP 初始化(显示/I2C/电量/按键) → UI 首屏 → WiFi(直连或配网)
//       → 数据 worker 周期拉取。
// 按键语义:
//   上/下 短按   列表中=移动选中;详情页=滚动;配网页无操作
//   确定  短按   列表中=进入详情;配网页=返回列表
//   确定  长按   列表中=打开配网;详情页=返回列表
// (沿用仓库惯例:回调只入队,处理在独立任务;非 LVGL 任务持 bsp_lvgl_lock)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "app_data.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;

static bool input_to_key(const input_event_t *in, app_ui_key_t *key) {
    if (in->event == BSP_BTN_LONG) {
        if (in->btn == BSP_BTN_OK) {
            *key = APP_UI_KEY_OK_LONG;
            return true;
        }
        return false;                        // 上下长按暂无语义
    }
    if (in->event != BSP_BTN_CLICK) return false;
    switch (in->btn) {
    case BSP_BTN_UP:   *key = APP_UI_KEY_UP;   return true;
    case BSP_BTN_DOWN: *key = APP_UI_KEY_DOWN; return true;
    case BSP_BTN_OK:   *key = APP_UI_KEY_OK;   return true;
    default:           return false;
    }
}

static void process_input(const input_event_t *input) {
    app_ui_key_t key;
    if (!input_to_key(input, &key)) return;
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "LVGL 锁超时,丢弃按键");
        return;
    }
    app_ui_key(key);
    bsp_lvgl_unlock();
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "VCT 赛事看板启动");

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    // 电量计:失败不阻塞(顶栏不显示电量)
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败");
    }

    // 按键队列 + 处理任务
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue &&
        xTaskCreate(input_task, "app_input", 4096, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key, NULL) == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    // 首屏(UI 在主任务创建,之后所有 UI 访问都持锁)
    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
    }

    // 网络与数据(后台任务)
    app_data_init();
#ifndef APP_OFFLINE_PREVIEW
    app_wifi_init();
#endif

    ESP_LOGI(TAG, "就绪");
}
