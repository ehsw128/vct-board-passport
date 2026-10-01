#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_preview.h"
#include "app_state.h"

int main(void) {
    app_match_t matches[5];
    app_match_detail_t details[2];
    int count = app_preview_fill(matches, 5, details, 2);
    assert(count == 5);
    int done = 0, live = 0, upcoming = 0;
    for (int i = 0; i < count; i++) {
        done += matches[i].status == APP_STATUS_DONE;
        live += matches[i].status == APP_STATUS_LIVE;
        upcoming += matches[i].status == APP_STATUS_UPCOMING;
    }
    assert(done == 2 && live == 1 && upcoming == 2);

    app_dates_view_t dates;
    app_state_build_dates(matches, count, app_preview_now(), &dates);
    assert(dates.count == 4);
    assert(dates.selected >= 0);
    assert(strcmp(dates.dates[dates.selected].key, "2026-09-29") == 0);

    app_day_view_t day;
    app_state_build_day(matches, count, "2026-09-29", &day);
    assert(day.count == 2);
    assert(matches[day.raw_idx[0]].status == APP_STATUS_LIVE);
    assert(matches[day.raw_idx[1]].status == APP_STATUS_UPCOMING);
    assert(details[0].match.id == matches[0].id);
    assert(details[0].map_count == 3);
    puts("test_app_preview: PASS");
    return 0;
}
