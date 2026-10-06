#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "bms/bms_watchdog.h"

void bms_virtual_watchdog_init(struct bms_virtual_watchdog *w)
{
    if (w != NULL) {
        memset(w, 0, sizeof(*w));
    }
}

void bms_virtual_watchdog_set_error(struct bms_virtual_watchdog *w,
                                    int error)
{
    if (w != NULL) {
        w->injected_error = error;
    }
}

static int virtual_feed(void *context)
{
    struct bms_virtual_watchdog *w = context;

    if (w == NULL) {
        return -EINVAL;
    }

    if (w->injected_error != 0) {
        return w->injected_error;
    }

    w->feed_count++;
    w->last_feed_ms = k_uptime_get();
    return 0;
}

const struct bms_watchdog_backend_ops
    bms_virtual_watchdog_ops = {
        .feed = virtual_feed,
};
