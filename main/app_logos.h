// 自动生成随 app_logos.c 一同维护 —— 接口由 tools/gen_team_logos.py 固定。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 按 teamSpName 查内置 logo 下标；未收录返回 -1。 */
int app_logos_find(const char *sp);
int app_logos_count(void);
/* 返回 const lv_image_dsc_t*，调用方转型；越界返回 NULL。 */
const void *app_logos_dsc_24(int idx);
const void *app_logos_dsc_32(int idx);

#ifdef __cplusplus
}
#endif
