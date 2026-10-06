#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include "bms_watchdog_stm32.h"

#define WDT_NODE DT_NODELABEL(iwdg)

static int feed(void *context)
{
    struct bms_stm32_watchdog *w = context;
    if (w == NULL || w->dev == NULL || w->channel_id < 0) return -EINVAL;
    return wdt_feed(w->dev, w->channel_id);
}

const struct bms_watchdog_backend_ops bms_stm32_watchdog_ops = { .feed = feed };

int bms_stm32_watchdog_init(struct bms_stm32_watchdog *w, uint32_t timeout_ms)
{
    if (w == NULL || timeout_ms == 0U) return -EINVAL;
    w->dev = DEVICE_DT_GET(WDT_NODE);
    w->channel_id = -1;
    if (!device_is_ready(w->dev)) return -ENODEV;

    struct wdt_timeout_cfg cfg = {
        .window = { .min = 0U, .max = timeout_ms },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };
    int id = wdt_install_timeout(w->dev, &cfg);
    if (id < 0) return id;
    w->channel_id = id;

    int ret = wdt_setup(w->dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret == -ENOTSUP) {
        ret = wdt_setup(w->dev, 0U);
    }
    return ret;
}
