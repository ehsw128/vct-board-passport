// main/app_wifi.c —— WiFi STA / SoftAP 网页配网 / SNTP。
#include "app_wifi.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "app_data.h"
#include "app_form.h"

#define TAG "app_wifi"

#define NVS_NS          "vct"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define STA_MAX_RETRY   5
#define EV_STA_OK       0x1
#define EV_STA_FAIL     0x2

static char s_ssid[33];
static char s_pass[65];
static bool s_has_cred;
static bool s_provisioning;
static bool s_proving;           // 配网页提交的新凭据正在验证
static int s_retry;
static int s_fail_reason;        // 验证期间 STA 断开的 reason 码,展示在失败页
static volatile int s_verify_state;    // 0 空闲,1 验证中,2 成功,3 失败
static volatile int s_verify_progress; // 验证阶段对应的进度,不代表精确耗时
static volatile bool s_verify_task_active;  // 抑制 STA_START 事件的自动连接
static int s_diag_pre_total;     // 诊断:验证前主动扫描听到的 AP 总数
static int s_diag_pin_ch = -1;   // 诊断:锁定的目标信道(-1=未锁定)
static int s_diag_post_match = -1;  // 诊断:失败后重扫时目标出现次数(-1=未扫)
static uint8_t s_pin_bssid[6];   // 诊断:预扫匹配到的目标 BSSID
static int s_diag_strat_code[4]; // 诊断:四种连接策略各自的原因码(0=成功,-1=未试)
static char s_diag_strats[64];   // 诊断:策略结果字符串,屏显用
static bool s_sntp_started;
static httpd_handle_t s_httpd;
static EventGroupHandle_t s_ev;
static char s_ap_ssid[33];       // 当前配网热点名

bool app_wifi_is_sta_connected(void) {
    return s_ev && (xEventGroupGetBits(s_ev) & EV_STA_OK) != 0;
}

bool app_wifi_can_fetch(void) {
    if (!app_wifi_is_sta_connected()) return false;
    wifi_mode_t mode;
    return esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA;
}

bool app_wifi_is_provisioning(void) {
    return s_provisioning;
}

int app_wifi_verify_state(void) { return s_verify_state; }
int app_wifi_verify_progress(void) { return s_verify_progress; }

void app_wifi_get_ap_ssid(char *buf, int size) {
    snprintf(buf, size, "%s", s_provisioning ? s_ap_ssid : "");
}

bool app_wifi_sntp_synced(void) {
    if (!s_sntp_started) return false;
    // esp_sntp_get_sync_status() 读到 COMPLETED 会立即清零，UI 轮询会消费
    // 这个一次性状态。HTTPS 只需要持续有效的墙上时间。
    return time(NULL) >= 1735689600; // 2025-01-01 UTC
}

// ---------------------------------------------------------------- NVS

static bool load_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(s_ssid);
    bool ok = nvs_get_str(h, NVS_KEY_SSID, s_ssid, &len) == ESP_OK && len > 1;
    if (ok) {
        len = sizeof(s_pass);
        ok = nvs_get_str(h, NVS_KEY_PASS, s_pass, &len) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static bool save_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_set_str(h, NVS_KEY_SSID, s_ssid);
    esp_err_t e2 = nvs_set_str(h, NVS_KEY_PASS, s_pass);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK;
}

static void clear_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
    }
    s_has_cred = false;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
}

// ---------------------------------------------------------------- SNTP

static void sntp_start(void) {
    if (s_sntp_started) return;
    setenv("TZ", "CST-8", 1);            // POSIX TZ:东八区
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    // 多服务器轮询,提高国内可达性
    esp_sntp_setservername(0, "ntp.tencent.com");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_setservername(3, "pool.ntp.org");
    esp_sntp_set_sync_interval(30 * 60 * 1000);   // 30 分钟重校
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP 已启动");
}

// ---------------------------------------------------------------- 配网 HTTP

// 配网页:深色电竞风,移动端优先;JS 调 /scan 列出周围网络,选中后填密码
static const char PAGE_FORM[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>\n"
    "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>\n"
    "<title>VCT 赛事看板 · 配网</title><style>\n"
    ":root{--bg:#0f1420;--card:#161c2a;--line:#232c40;--ink:#e8ecf4;--sub:#8a93a6;--red:#ff4655}\n"
    "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}\n"
    "body{background:var(--bg);color:var(--ink);font:15px/1.5 -apple-system,'PingFang SC',"
    "'Microsoft YaHei',sans-serif;padding:24px 16px calc(24px + env(safe-area-inset-bottom))}\n"
    ".wrap{max-width:420px;margin:0 auto}\n"
    ".logo{display:flex;align-items:center;gap:10px;margin-bottom:6px}\n"
    ".logo i{width:10px;height:26px;background:linear-gradient(180deg,var(--red),#b8323c);"
    "border-radius:2px}\n"
    "h1{font-size:20px}.sub{color:var(--sub);font-size:13px;margin-bottom:18px}\n"
    ".panel{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}\n"
    ".row{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}\n"
    ".row b{font-size:14px}\n"
    "#refresh{color:var(--red);background:none;border:1px solid var(--line);border-radius:8px;"
    "padding:6px 12px;font-size:13px}\n"
    ".ap{width:100%;display:flex;align-items:center;gap:10px;background:none;border:0;"
    "border-bottom:1px solid var(--line);padding:12px 4px;color:var(--ink);font-size:15px;"
    "text-align:left}\n"
    ".ap:last-child{border-bottom:0}.ap.on{color:var(--red)}\n"
    ".ap .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
    ".sig{letter-spacing:1px;color:var(--sub);font-size:12px}.ap.on .sig{color:var(--red)}\n"
    ".lk{color:var(--sub);flex:none}.ap.on .lk{color:var(--red)}\n"
    "label{display:block;font-size:13px;color:var(--sub);margin:14px 0 6px}\n"
    "input{width:100%;background:#0b0f18;border:1px solid var(--line);border-radius:10px;"
    "color:var(--ink);padding:12px;font-size:16px}\n"
    "input:focus{outline:none;border-color:var(--red)}\n"
    "button.go{width:100%;margin-top:18px;background:linear-gradient(135deg,var(--red),#d93a47);"
    "border:0;border-radius:12px;color:#fff;font-size:16px;font-weight:600;padding:14px}\n"
    "button.go[disabled]{opacity:.6}\n"
    ".tip{color:var(--sub);font-size:12px;margin-top:12px;text-align:center}\n"
    "</style></head><body><div class=wrap>\n"
    "<div class=logo><i></i><h1>VCT 赛事看板</h1></div>\n"
    "<p class=sub>选择下方网络或手动输入,为设备配置 Wi-Fi(仅支持 2.4GHz)</p>\n"
    "<div class=panel>\n"
    "<div class=row><b>附近的网络</b><button id=refresh type=button>重新扫描</button></div>\n"
    "<div id=list><p class=sub style='padding:12px 4px'>正在扫描…</p></div>\n"
    "<label for=ssid>Wi-Fi 名称</label><input id=ssid maxlength=32 autocomplete=off>\n"
    "<label for=pass>密码</label><input id=pass type=password maxlength=64>\n"
    "<button class=go id=go type=button>连 接</button>\n"
    "</div>\n"
    "<p class=tip>连接成功后,设备屏幕会自动进入赛程页面</p>\n"
    "</div><script>\n"
    "var sel='';\n"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')}\n"
    "function bars(q){return q>=-55?'\\u2582\\u2584\\u2586\\u2588':q>=-70?'\\u2582\\u2584\\u2586'\n"
    "  :q>=-82?'\\u2582\\u2584':'\\u2582'}\n"
    "function lk(){return '<svg class=lk width=10 height=12 viewBox=\"0 0 10 12\">'\n"
    " +'<path fill=currentColor fill-rule=evenodd '\n"
    " +'d=\"M3 5V3.5a2 2 0 014 0V5h1.5v7h-7V5H3zM4 5h2V3.5a1 1 0 00-2 0V5z\"/></svg>'}\n"
    "function render(list){\n"
    " var el=document.getElementById('list');el.innerHTML='';\n"
    " if(!list.length){el.innerHTML='<p class=sub style=padding:12px 4px>未发现网络,请重新扫描</p>';return}\n"
    " list.forEach(function(a){\n"
    "  var b=document.createElement('button');b.type='button';\n"
    "  b.className='ap'+(a.s==sel?' on':'');\n"
    "  b.innerHTML='<span class=nm>'+esc(a.s)+'</span><span class=sig>'+bars(a.q)+'</span>'\n"
    "   +(a.l?lk():'');\n"
    "  b.onclick=function(){sel=a.s;document.getElementById('ssid').value=a.s;\n"
    "   document.getElementById('pass').focus();render(list)};\n"
    "  el.appendChild(b)})}\n"
    "function scan(){\n"
    " var el=document.getElementById('list');\n"
    " el.innerHTML='<p class=sub style=padding:12px 4px>正在扫描…</p>';\n"
    " fetch('/scan').then(function(r){return r.json()})\n"
    "  .then(function(d){render(d.ap||[])})\n"
    "  .catch(function(){el.innerHTML='<p class=sub style=padding:12px 4px>扫描失败,请重试</p>'})}\n"
    "document.getElementById('refresh').onclick=scan;scan();\n"
    "document.getElementById('go').onclick=function(){\n"
    " var s=document.getElementById('ssid').value.trim();\n"
    " if(!s)return;\n"
    " var b=this;b.disabled=true;b.textContent='连接中…';\n"
    " fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
    "  body:new URLSearchParams({ssid:s,pass:document.getElementById('pass').value})})\n"
    "  .then(function(r){return r.text()})\n"
    "  .then(function(t){document.open();document.write(t);document.close()})\n"
    "  .catch(function(){b.disabled=false;b.textContent='连 接'})}\n"
    "</script></body></html>\n";

// 提交后的等待页:热点保持开启,按真实验证阶段更新进度。
static const char PAGE_VERIFY[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<title>VCT 看板 · 配网进度</title><style>"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;"
    "background:#0f1420;color:#e8ecf4;font:16px/1.6 -apple-system,'PingFang SC','Microsoft YaHei',sans-serif;"
    "padding:24px}main{width:min(100%,420px);background:#161c2a;border:1px solid #30394d;"
    "border-radius:18px;padding:26px}small{color:#8a93a6}h1{font-size:22px;margin:8px 0 12px}"
    ".track{height:12px;background:#30394d;border-radius:12px;overflow:hidden;margin:22px 0 10px}"
    "#bar{height:100%;width:5%;background:#ff4655;transition:width .5s}"
    "#pct{float:right;color:#ff6874}#msg{min-height:52px;color:#abb5c8}"
    "#result{font-weight:600}a{color:#ff6874}"
    "</style></head><body><main><small>VCT 赛事看板 / WI-FI</small>"
    "<h1 id=title>正在验证网络</h1><div class=track><div id=bar></div></div>"
    "<span id=pct>5%</span><p id=msg>设备正在扫描目标网络，热点保持开启。</p>"
    "<p id=result></p><small>若手机因热点信道切换而短暂断开，请重新连接看板热点；验证会继续。</small>"
    "<script>var lost=0;function poll(){fetch('/status',{cache:'no-store'}).then(function(r){return r.text()})"
    ".then(function(t){var a=t.split(','),s=+a[0],p=Math.max(0,Math.min(100,+a[1]||0));"
    "document.getElementById('bar').style.width=p+'%';document.getElementById('pct').textContent=p+'%';"
    "var m=document.getElementById('msg'),h=document.getElementById('title'),o=document.getElementById('result');"
    "if(s===2){h.textContent='配网成功';m.textContent='已连接 Wi-Fi，正在同步赛程。';"
    "o.textContent='可以返回看板查看比赛。';return}"
    "if(s===3){h.textContent='连接失败';m.textContent='原因码：'+(a[2]||'0');"
    "o.innerHTML='<a href=\"/fail\">查看原因</a> · <a href=\"/\">重新选择网络</a>';return}"
    "m.textContent=p<30?'正在扫描目标网络…':p<90?'正在尝试连接路由器…':'已连接，正在保存配置…';"
    "lost=0;setTimeout(poll,1200)})"
    ".catch(function(){if(++lost>2)document.getElementById('msg').textContent="
    "'热点连接暂时中断，请检查手机 Wi-Fi 后重试。';setTimeout(poll,2000)})}poll()"
    "</script></main></body></html>";

// 把 ESP-IDF 断开原因码翻译成可操作的中文提示
static const char *fail_reason_text(int reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "找不到该 Wi-Fi:请确认名称正确,路由器开启了 2.4GHz 广播";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "认证失败:密码可能不正确";
    case WIFI_REASON_ASSOC_FAIL:
        return "关联被拒绝:路由器可能限制了设备数量或开启了过滤";
    case 0:
        return "连接超时:路由器响应慢,或设备离路由器太远";
    case -1:
        return "表单数据不完整,请返回重填";
    case -3:
        return "网络已连通,但保存配置失败,请重试";
    default:
        return "连接失败";
    }
}

// 带原因的失败页
static esp_err_t http_send(httpd_req_t *req, const char *body);
static esp_err_t http_send_fail(httpd_req_t *req, int reason) {
    char page[416];
    snprintf(page, sizeof(page),
             "<!doctype html><html><head><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,initial-scale=1'>"
             "<title>连接失败</title></head>"
             "<body style='font-family:sans-serif;max-width:480px;margin:48px auto'>"
             "<h2 style='color:#c0392b'>连接失败</h2><p>%s</p>"
             "<p style='color:#888'>原因码:%d</p>"
             "<p><a href='/'>返回重试</a></p></body></html>",
             fail_reason_text(reason), reason);
    return http_send(req, page);
}

static esp_err_t http_send(httpd_req_t *req, const char *body) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_form(httpd_req_t *req) {
    return http_send(req, PAGE_FORM);
}

// SSID 转 JSON 字符串(转义 \ " 和控制字符);httpd 单任务调用,静态缓冲安全
static const char *json_escape_ssid(const char *s) {
    static char out[71];                     // 32B SSID 转义后最长约 65+1
    char *w = out;
    for (const char *r = s; *r && w < out + sizeof(out) - 2; r++) {
        if (*r == '"' || *r == '\\') *w++ = '\\';
        *w++ = (unsigned char)*r < 0x20 ? ' ' : *r;
    }
    *w = '\0';
    return out;
}

// GET /scan —— 设备硬件扫描周围 2.4GHz 网络,按信号强度返回 JSON
static esp_err_t handler_scan(httpd_req_t *req) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);  // 扫描需要 STA 能力,不影响热点
    }
    wifi_scan_config_t sc = { 0 };           // 默认全信道,结果按 RSSI 降序
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ap\":[]}");
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 16) num = 16;                  // 看板场景取最近 16 个足够
    wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ap\":[]}");
    }
    esp_wifi_scan_get_ap_records(&num, recs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ap\":[");
    char item[128];
    bool first = true;
    for (int i = 0; i < num; i++) {
        recs[i].ssid[32] = '\0';
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;              // 隐藏网络无法手选,跳过
        bool dup = false;
        for (int j = 0; j < i; j++) {        // 同名多 BSSID 只留最强
            if (strcmp((const char *)recs[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        int n = snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"q\":%d,\"l\":%d}",
                         first ? "" : ",", json_escape_ssid(ssid),
                         (int)recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN);
        if (n < 0 || n >= (int)sizeof(item)) continue;
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);     // 结束分块响应
    free(recs);
    return ESP_OK;
}

// 兜底重定向:未注册路径一律 302 到配网页。
// 手机连热点后系统探测 generate_204 / captive.apple.com 等会命中这里,
// 从而自动弹出"登录到网络"提示和配网页(captive portal)。
static esp_err_t handler_captive(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_sendstr(req, "");
}

// 迷你 DNS:AP 内所有域名都解析到 192.168.4.1,配合上面的重定向触发系统弹窗。
// 仅监听 AP 接口;STA 正常联网后热点关闭,不会再收到查询。
static void captive_dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "captive DNS 启动失败");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || n >= (int)sizeof(buf) - 16) continue;
        if (buf[2] & 0x80) continue;         // 只处理查询,忽略响应包
        // 找问题区结尾:12 字节头后是 QNAME(以 NUL 结尾)+ QTYPE/QCLASS(4B)
        int q_end = 12;
        while (q_end < n && buf[q_end]) q_end++;
        q_end += 5;
        if (q_end > n) continue;
        // 复用原包:保留 ID 与问题区,置响应标志,QD=1,AN=1
        buf[2] = 0x81; buf[3] = 0x80;        // 响应+递归可用
        buf[4] = 0; buf[5] = 1;              // QDCOUNT=1
        buf[6] = 0; buf[7] = 1;              // ANCOUNT=1
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        uint8_t *an = buf + q_end;           // 答案区:指向问题名的 A 记录
        an[0] = 0xC0; an[1] = 0x0C;
        an[2] = 0; an[3] = 1;                // Type A
        an[4] = 0; an[5] = 1;                // Class IN
        an[6] = 0; an[7] = 0; an[8] = 0; an[9] = 60;   // TTL 60s
        an[10] = 0; an[11] = 4;              // RDLENGTH
        an[12] = 192; an[13] = 168; an[14] = 4; an[15] = 1;
        (void)sendto(sock, buf, q_end + 16, 0, (struct sockaddr *)&from, flen);
    }
}

static esp_err_t handler_clear(httpd_req_t *req) {
    clear_credentials();
    ESP_LOGI(TAG, "已清除存档凭据");
    return http_send(req, PAGE_FORM);
}

// ---------------------------------------------------------------- 提交验证
// 独立任务:热点保持开启,通过 AP+STA 验证新凭据。
static void verify_task(void *arg) {
    (void)arg;
    vTaskDelay(300 / portTICK_PERIOD_MS);        // 等待页响应发出
    s_verify_task_active = true;                 // 抑制 STA_START 自动连接
    s_proving = true;                            // 复用断开原因捕获逻辑
    s_verify_state = 1;
    s_verify_progress = 10;
    s_fail_reason = 0;
    s_retry = STA_MAX_RETRY;                     // 验证期间不自动重连
    xEventGroupClearBits(s_ev, EV_STA_OK | EV_STA_FAIL);

    esp_wifi_set_mode(WIFI_MODE_APSTA);          // 验证期间保留手机与设备的连接
    esp_wifi_disconnect();                       // 在线改网时先断开旧 STA,AP 不受影响
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.rssi = -127;               // 显式关闭 RSSI 门限
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // 显式不限加密方式
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    // 诊断:验证前主动扫一遍,记录听到的 AP 数;匹配则锁定 BSSID+信道直连
    s_diag_pre_total = 0;
    s_diag_pin_ch = -1;
    s_verify_progress = 20;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        s_diag_pre_total = n;
        if (n > 24) n = 24;
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs) {
            esp_wifi_scan_get_ap_records(&n, recs);
            for (int i = 0; i < n; i++) {
                recs[i].ssid[32] = '\0';
                if (strcmp(s_ssid, (const char *)recs[i].ssid) == 0) {
                    cfg.sta.bssid_set = true;
                    memcpy(cfg.sta.bssid, recs[i].bssid, sizeof(recs[i].bssid));
                    memcpy(s_pin_bssid, recs[i].bssid, sizeof(s_pin_bssid));
                    cfg.sta.channel = recs[i].primary;
                    s_diag_pin_ch = recs[i].primary;
                    ESP_LOGI(TAG, "锁定目标信道 %d", recs[i].primary);
                    esp_wifi_set_config(WIFI_IF_STA, &cfg);
                    break;
                }
            }
            free(recs);
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    s_verify_progress = 35;
    vTaskDelay(200 / portTICK_PERIOD_MS);        // 等 STA_START 事件落地

    // 多策略连击:每种连接方式各试一次,全部记录原因码,一次刷机拿全对比数据
    static const struct { bool pin; uint8_t chan; uint8_t method; char tag; } strats[] = {
        { true,  1, WIFI_ALL_CHANNEL_SCAN, 'P' },   // 锁 BSSID+信道
        { false, 0, WIFI_ALL_CHANNEL_SCAN, 'A' },   // 全信道扫描
        { false, 0, WIFI_FAST_SCAN,        'F' },   // 快速扫描
        { false, 1, WIFI_ALL_CHANNEL_SCAN, 'C' },   // 仅锁信道
    };
    for (int i = 0; i < 4; i++) s_diag_strat_code[i] = -1;
    EventBits_t bits = 0;
    int ok_strat = -1;
    for (int si = 0; si < 4 && ok_strat < 0; si++) {
        s_verify_progress = 40 + si * 12;
        cfg.sta.bssid_set = strats[si].pin && (s_diag_pin_ch > 0);
        if (cfg.sta.bssid_set) memcpy(cfg.sta.bssid, s_pin_bssid, 6);
        cfg.sta.channel = strats[si].chan ? s_diag_pin_ch : 0;
        cfg.sta.scan_method = strats[si].method;
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        xEventGroupClearBits(s_ev, EV_STA_OK | EV_STA_FAIL);
        s_fail_reason = 0;
        vTaskDelay(100 / portTICK_PERIOD_MS);
        esp_err_t cr = esp_wifi_connect();
        if (cr != ESP_OK) {
            s_diag_strat_code[si] = 800 + (int)cr;   // 连接 API 直接报错
            s_fail_reason = s_diag_strat_code[si];
            continue;
        }
        bits = xEventGroupWaitBits(s_ev, EV_STA_OK | EV_STA_FAIL,
                                   pdFALSE, pdFALSE,
                                   10000 / portTICK_PERIOD_MS);
        if (bits & EV_STA_OK) {
            s_diag_strat_code[si] = 0;
            ok_strat = si;
            ESP_LOGI(TAG, "策略 %c 连接成功", strats[si].tag);
            break;
        }
        s_diag_strat_code[si] = (bits & EV_STA_FAIL) ? s_fail_reason : -2;
        if (!(bits & EV_STA_FAIL)) s_fail_reason = -2;
        // 已收到断开事件时无需再次主动断开；超时才停止本轮连接。
        if (!(bits & EV_STA_FAIL)) esp_wifi_disconnect();
        vTaskDelay(150 / portTICK_PERIOD_MS);
    }
    snprintf(s_diag_strats, sizeof(s_diag_strats), "%c:%d %c:%d %c:%d %c:%d",
             strats[0].tag, s_diag_strat_code[0], strats[1].tag, s_diag_strat_code[1],
             strats[2].tag, s_diag_strat_code[2], strats[3].tag, s_diag_strat_code[3]);
    if (ok_strat >= 0) {
        s_verify_progress = 95;
        s_proving = false;
        s_verify_task_active = false;
        if (!save_credentials()) {
            s_fail_reason = -3;
            s_verify_state = 3;
            s_verify_progress = 100;
            ESP_LOGE(TAG, "新凭据写入 NVS 失败");
            vTaskDelete(NULL);
            return;
        }
        s_has_cred = true;
        s_retry = 0;
        s_fail_reason = 0;
        s_verify_state = 2;
        s_verify_progress = 100;
        s_provisioning = false;
        ESP_LOGI(TAG, "新凭据验证成功,已写入 NVS");
        // 手机至少能轮询数次成功反馈;随后让 STA 独占射频进行 HTTPS 拉取。
        vTaskDelay(8000 / portTICK_PERIOD_MS);
        if (!s_provisioning && s_verify_state == 2) {
            if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "新凭据验证失败");
    esp_wifi_disconnect();
    // 诊断:失败后再扫一遍,看目标是否仍在射频范围内
    s_diag_post_match = -1;
    wifi_scan_config_t sc2 = { 0 };
    if (esp_wifi_scan_start(&sc2, true) == ESP_OK) {
        uint16_t n2 = 0;
        esp_wifi_scan_get_ap_num(&n2);
        wifi_ap_record_t *r2 = calloc(n2 ? n2 : 1, sizeof(*r2));
        if (r2) {
            esp_wifi_scan_get_ap_records(&n2, r2);
            int m = 0;
            for (int i = 0; i < n2; i++) {
                r2[i].ssid[32] = '\0';
                if (strcmp(s_ssid, (const char *)r2[i].ssid) == 0) m++;
            }
            free(r2);
            s_diag_post_match = m;
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    s_verify_state = 3;                          // 热点保留,可直接重试
    s_verify_progress = 100;
    s_proving = false;
    s_verify_task_active = false;
    vTaskDelete(NULL);
}

// GET /status —— 首字节保持兼容,其后为进度和原因码。
static esp_err_t handler_status(httpd_req_t *req) {
    char st[32];
    snprintf(st, sizeof(st), "%d,%d,%d", s_verify_state,
             s_verify_progress, s_fail_reason);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, st);
}

// GET /fail —— 显示失败原因
static esp_err_t handler_fail(httpd_req_t *req) {
    return http_send_fail(req, s_fail_reason);
}

static esp_err_t handler_save(httpd_req_t *req) {
    char body[512];
    if (req->content_len >= sizeof(body)) return http_send_fail(req, -1);
    size_t total = 0;
    int n;
    while (total < req->content_len &&
           (n = httpd_req_recv(req, body + total, req->content_len - total)) > 0) {
        total += n;
    }
    if (total != req->content_len) return http_send_fail(req, -1);
    body[total] = '\0';

    char ssid[33], pass[65];
    if (!app_form_get_field(body, (size_t)total, "ssid", ssid, sizeof(ssid)) ||
        ssid[0] == '\0' ||
        !app_form_get_field(body, (size_t)total, "pass", pass, sizeof(pass))) {
        return http_send_fail(req, -1);
    }
    if (s_verify_state == 1) return http_send(req, PAGE_VERIFY);
    // 密码不进日志
    ESP_LOGI(TAG, "配网提交: ssid=%s pass_len=%d", ssid, (int)strlen(pass));

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';

    s_verify_state = 1;
    s_verify_progress = 5;
    http_send(req, PAGE_VERIFY);                 // 先把等待页发出去再动射频
    if (xTaskCreate(verify_task, "wifi_verify", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "验证任务创建失败");
        s_verify_state = 3;                      // 轮询页会跳到 /fail
        s_verify_progress = 100;
    }
    return ESP_OK;
}

static void start_webserver(void) {
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // 支持通配;精确路径按注册顺序优先命中
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        httpd_uri_t u_form = { .uri = "/", .method = HTTP_GET, .handler = handler_form };
        httpd_uri_t u_scan = { .uri = "/scan", .method = HTTP_GET, .handler = handler_scan };
        httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = handler_save };
        httpd_uri_t u_clear = { .uri = "/clear", .method = HTTP_POST, .handler = handler_clear };
        httpd_uri_t u_status = { .uri = "/status", .method = HTTP_GET, .handler = handler_status };
        httpd_uri_t u_fail = { .uri = "/fail", .method = HTTP_GET, .handler = handler_fail };
        httpd_register_uri_handler(s_httpd, &u_form);
        httpd_register_uri_handler(s_httpd, &u_scan);
        httpd_register_uri_handler(s_httpd, &u_save);
        httpd_register_uri_handler(s_httpd, &u_clear);
        httpd_register_uri_handler(s_httpd, &u_status);
        httpd_register_uri_handler(s_httpd, &u_fail);
        // 通配兜底放最后注册:精确路径(/、/scan、/save、/clear)优先匹配
        httpd_uri_t u_captive = { .uri = "/*", .method = HTTP_GET, .handler = handler_captive };
        httpd_register_uri_handler(s_httpd, &u_captive);
        if (xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "captive DNS 任务创建失败");
        }
        ESP_LOGI(TAG, "配网页面已启动 http://192.168.4.1");
    } else {
        ESP_LOGE(TAG, "httpd 启动失败");
    }
}

// ---------------------------------------------------------------- 事件

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_verify_task_active && s_has_cred && !s_provisioning) {
            esp_wifi_connect();                  // 验证任务自己控制连接时机
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_STA_OK);
        if (s_proving) {
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)data;
            if (d && d->reason == WIFI_REASON_STA_LEAVING) return;
            s_fail_reason = d ? (int)d->reason : 0;
            ESP_LOGW(TAG, "验证断开,reason=%d", s_fail_reason);
            xEventGroupSetBits(s_ev, EV_STA_FAIL);
            return;
        }
        if (s_provisioning) return;
        if (s_retry < STA_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "STA 断开,重试 %d/%d", s_retry, STA_MAX_RETRY);
            esp_wifi_connect();
        } else if (!s_provisioning) {
            ESP_LOGW(TAG, "重试耗尽,转入配网模式");
            app_wifi_start_provisioning();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_ev, EV_STA_OK);
        xEventGroupClearBits(s_ev, EV_STA_FAIL);
        sntp_start();
        app_data_request_refresh();
        ESP_LOGI(TAG, "STA 已连接");
    }
}

// ---------------------------------------------------------------- 启动

void app_wifi_start_provisioning(void) {
    if (s_provisioning) return;
    s_provisioning = true;
    s_verify_state = 0;
    s_verify_progress = 0;
    s_fail_reason = 0;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "VCTBoard-%02X%02X",
             mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", (char *)ap.ap.ssid);
    ap.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    start_webserver();
    ESP_LOGI(TAG, "热点 %s 已开启", (char *)ap.ap.ssid);
}

int app_wifi_last_fail_reason(void) {
    return s_fail_reason;
}

void app_wifi_diag_info(int *pre_total, int *pin_ch, int *post_match) {
    *pre_total = s_diag_pre_total;
    *pin_ch = s_diag_pin_ch;
    *post_match = s_diag_post_match;
}

int app_wifi_get_target_ssid(char *buf, int size) {
    if (!buf || size <= 0) return 0;
    strncpy(buf, s_ssid, size - 1);
    buf[size - 1] = '\0';
    return (int)strlen(s_ssid);
}

const char *app_wifi_diag_strats(void) {
    return s_diag_strats;
}

void app_wifi_diag_pin_mac(char *buf, int size) {
    if (!buf || size <= 0) return;
    if (s_diag_pin_ch > 0) {
        snprintf(buf, size, "AP:" MACSTR, MAC2STR(s_pin_bssid));
    } else {
        buf[0] = '\0';
    }
}

void app_wifi_init(void) {
    // WiFi 驱动依赖 NVS;整片刷机后分区内容可能失效,按官方流程重初始化
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    // 库默认国家策略"01"只主动扫信道 1~11,路由器若在 12/13 信道会
    // 报 NO_AP_FOUND(201)。强制中国区策略,允许主动扫描 1~13。
    esp_wifi_set_country_code("CN", true);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();

    s_has_cred = load_credentials();
    if (s_has_cred) {
        wifi_config_t sta = { 0 };
        strncpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password) - 1);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ESP_LOGI(TAG, "使用存档凭据连接: %s", s_ssid);
    } else {
        ESP_LOGI(TAG, "无存档凭据,进入配网模式");
        app_wifi_start_provisioning();
    }
}
