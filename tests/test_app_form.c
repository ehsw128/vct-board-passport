#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_form.h"

int main(void) {
    char ssid[33], pass[65];
    const char *body = "ssid=%E6%B5%8B%E8%AF%95%E7%BD%91%E7%BB%9C&pass=a%26b%2Bc+de";
    assert(app_form_get_field(body, strlen(body), "ssid", ssid, sizeof(ssid)));
    assert(strcmp(ssid, "测试网络") == 0);
    assert(app_form_get_field(body, strlen(body), "pass", pass, sizeof(pass)));
    assert(strcmp(pass, "a&b+c de") == 0);

    assert(!app_form_get_field("ssid=%E6%8A%", strlen("ssid=%E6%8A%"),
                               "ssid", ssid, sizeof(ssid)));
    assert(!app_form_get_field("ssid=%GG", strlen("ssid=%GG"),
                               "ssid", ssid, sizeof(ssid)));
    assert(!app_form_get_field("ssid=abcdef", strlen("ssid=abcdef"),
                               "ssid", ssid, 4));
    assert(!app_form_get_field("other=x", strlen("other=x"),
                               "ssid", ssid, sizeof(ssid)));
    assert(app_form_get_field("ssid=x&pass=", strlen("ssid=x&pass="),
                              "pass", pass, sizeof(pass)));
    assert(pass[0] == '\0');
    puts("test_app_form: PASS");
    return 0;
}
