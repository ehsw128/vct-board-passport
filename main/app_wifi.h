// main/app_wifi.h —— WiFi STA / SoftAP 网页配网 / SNTP 校时。
//
// 启动策略:
//   NVS 有凭据 -> STA 直连;连续失败 5 次转配网。
//   无凭据     -> 直接开 SoftAP("VCTBoard-XXXX",无密码),手机连接后
//                 访问 http://192.168.4.1 填写家庭 WiFi。
// 新凭据在 AP+STA 模式下实测连上后才写入 NVS(旧凭据保留至验证成功)。
// 任何日志不得打印 WiFi 密码。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_wifi_init(void);          // nvs_flash_init + 读凭据 + 启动 STA 或配网

// 主动进入配网模式(主页 OK 长按入口;STA 保持连接时也可开 AP 供改网)。
void app_wifi_start_provisioning(void);

bool app_wifi_is_sta_connected(void);
bool app_wifi_can_fetch(void);      // STA 已连通且配网热点已收起,可开始 HTTPS
bool app_wifi_is_provisioning(void);
int app_wifi_verify_state(void);   // 0 待提交,1 验证中,2 成功,3 失败
int app_wifi_verify_progress(void); // 0..100,由真实验证阶段推进
bool app_wifi_sntp_synced(void);   // 时间是否已同步(UI 决定显示 --:-- 还是 HH:MM)

// 当前配网热点名(未开配网时为空串)。UI 显示用。
void app_wifi_get_ap_ssid(char *buf, int size);

// 最近一次配网验证失败的原因码(0=无失败,2 成功后清零)。UI 屏显诊断用。
int app_wifi_last_fail_reason(void);

// 配网验证诊断(屏显):验证前听到 AP 总数 / 锁定信道(-1=未锁定) / 失败后重扫目标出现次数(-1=未扫)
void app_wifi_diag_info(int *pre_total, int *pin_ch, int *post_match);

// 验证时设备实际收到的目标 SSID,返回字节长度。屏显核对用。
int app_wifi_get_target_ssid(char *buf, int size);

// 四种连接策略的结果串("P:201 A:201 F:201 C:201",0=成功 -1=未试)。屏显用。
const char *app_wifi_diag_strats(void);

// 预扫匹配到的目标 AP MAC("AP:xx:xx:xx:xx:xx:xx"),未匹配时空串。屏显用。
void app_wifi_diag_pin_mac(char *buf, int size);

#ifdef __cplusplus
}
#endif
