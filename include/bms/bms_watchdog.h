#ifndef BMS_WATCHDOG_H_
#define BMS_WATCHDOG_H_

#include <stdint.h>

struct bms_watchdog_backend_ops {
    int (*feed)(void *context);
};

struct bms_virtual_watchdog {
    uint32_t feed_count;
    int64_t last_feed_ms;
    int injected_error;
};

void bms_virtual_watchdog_init(struct bms_virtual_watchdog *);
void bms_virtual_watchdog_set_error(struct bms_virtual_watchdog *, int error);

extern const struct bms_watchdog_backend_ops
    bms_virtual_watchdog_ops;

#endif
