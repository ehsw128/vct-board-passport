idf_component_register(
    SRCS "main.c"
         "app_json.c"
         "app_state.c"
         "app_data.c"
         "app_wifi.c"
         "app_form.c"
         "app_ui.c"
         "app_logos.c"
         "app_brand.c"
         "app_map_icons.c"
         "app_preview.c"
         "../assets/fonts/noto_sc_16.c"
         "../assets/fonts/noto_sc_22.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp_http_server esp-tls mbedtls
)

# 生成的字体文件用 #ifdef LV_LVGL_H_INCLUDE_SIMPLE 守卫,此处启用以包含 "lvgl.h"
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

option(APP_OFFLINE_PREVIEW "Build the VCT board with embedded offline UI fixtures" OFF)
if(APP_OFFLINE_PREVIEW)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OFFLINE_PREVIEW=1)
endif()
