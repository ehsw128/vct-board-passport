#include "app_form.h"

#include <string.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool app_form_get_field(const char *body, size_t body_len, const char *field,
                        char *out, size_t out_size) {
    if (!body || !field || !out || out_size == 0) return false;
    out[0] = '\0';
    size_t field_len = strlen(field);
    for (size_t pos = 0; pos < body_len;) {
        size_t end = pos;
        while (end < body_len && body[end] != '&') end++;
        if (end > pos + field_len &&
            memcmp(body + pos, field, field_len) == 0 &&
            body[pos + field_len] == '=') {
            size_t written = 0;
            for (size_t i = pos + field_len + 1; i < end; i++) {
                unsigned char ch = (unsigned char)body[i];
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%') {
                    if (i + 2 >= end) return false;
                    int hi = hex_digit(body[i + 1]);
                    int lo = hex_digit(body[i + 2]);
                    if (hi < 0 || lo < 0) return false;
                    ch = (unsigned char)((hi << 4) | lo);
                    i += 2;
                }
                if (ch == 0 || written + 1 >= out_size) {
                    out[0] = '\0';
                    return false;
                }
                out[written++] = (char)ch;
            }
            out[written] = '\0';
            return true;
        }
        pos = end + 1;
    }
    return false;
}
