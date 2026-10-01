#pragma once

#include <stdbool.h>
#include <stddef.h>

// Decode one application/x-www-form-urlencoded field into a bounded byte buffer.
// Returns false for a missing field, malformed escape, or decoded value overflow.
bool app_form_get_field(const char *body, size_t body_len, const char *field,
                        char *out, size_t out_size);
