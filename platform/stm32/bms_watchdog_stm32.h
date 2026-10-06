#ifndef BMS_WATCHDOG_STM32_H_
#define BMS_WATCHDOG_STM32_H_

#include <stdint.h>
#include <zephyr/device.h>
#include "bms/bms_watchdog.h"

struct bms_stm32_watchdog {
    const struct device *dev;
    int channel_id;
};

int bms_stm32_watchdog_init(struct bms_stm32_watchdog *, uint32_t timeout_ms);
extern const struct bms_watchdog_backend_ops bms_stm32_watchdog_ops;

#endif
