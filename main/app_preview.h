#pragma once

#include <stdint.h>
#include "app_model.h"

// Offline, synthetic matches used only by the simulator preview build.
int app_preview_fill(app_match_t *matches, int match_cap,
                     app_match_detail_t *details, int detail_cap);
int64_t app_preview_now(void);
