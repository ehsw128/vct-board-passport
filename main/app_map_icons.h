// 自动生成随 app_map_icons.c 一同维护 —— 接口由 tools/gen_map_icons.py 固定。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 按英文地图名(mapNameEn)查图标下标；未收录返回 -1。 */
int app_map_icons_find(const char *en);
int app_map_icons_count(void);
/* 返回 const lv_image_dsc_t*；越界返回 NULL。 */
const void *app_map_icons_dsc(int idx);

#ifdef __cplusplus
}
#endif
