#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "bms_board_stm32.h"

#define BQ0_NODE DT_NODELABEL(bq0)
#define BOARD_IO_NODE DT_NODELABEL(bms_board_io)

static const struct gpio_dt_spec bq0_alert = GPIO_DT_SPEC_GET(BQ0_NODE, alert_gpios);
static const struct gpio_dt_spec bq0_rst = GPIO_DT_SPEC_GET(BQ0_NODE, rst_shut_gpios);
static const struct gpio_dt_spec bq0_dfet = GPIO_DT_SPEC_GET(BQ0_NODE, dfetoff_gpios);

#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
#define BQ1_NODE DT_NODELABEL(bq1)
static const struct gpio_dt_spec bq1_alert = GPIO_DT_SPEC_GET(BQ1_NODE, alert_gpios);
static const struct gpio_dt_spec bq1_rst = GPIO_DT_SPEC_GET(BQ1_NODE, rst_shut_gpios);
static const struct gpio_dt_spec bq1_dfet = GPIO_DT_SPEC_GET(BQ1_NODE, dfetoff_gpios);
#endif

static const struct gpio_dt_spec rs485_de = GPIO_DT_SPEC_GET(BOARD_IO_NODE, rs485_de_re_gpios);
#if DT_NODE_HAS_PROP(BOARD_IO_NODE, buzzer_gpios)
static const struct gpio_dt_spec buzzer = GPIO_DT_SPEC_GET(BOARD_IO_NODE, buzzer_gpios);
#endif
#if DT_NODE_HAS_PROP(BOARD_IO_NODE, start_gpios)
static const struct gpio_dt_spec start_input = GPIO_DT_SPEC_GET(BOARD_IO_NODE, start_gpios);
#endif

static struct gpio_callback bq0_alert_cb;
#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
static struct gpio_callback bq1_alert_cb;
#endif
static atomic_t alert_count[2];

static void alert0_handler(const struct device *port, struct gpio_callback *cb,
                           gpio_port_pins_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    atomic_inc(&alert_count[0]);
}

#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
static void alert1_handler(const struct device *port, struct gpio_callback *cb,
                           gpio_port_pins_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    atomic_inc(&alert_count[1]);
}
#endif

static int require_gpio(const struct gpio_dt_spec *s)
{
    return gpio_is_ready_dt(s) ? 0 : -ENODEV;
}

static int init_bq_gpio(const struct gpio_dt_spec *alert,
                        const struct gpio_dt_spec *rst,
                        const struct gpio_dt_spec *dfet,
                        struct gpio_callback *cb,
                        gpio_callback_handler_t handler)
{
    int ret = require_gpio(alert); if (ret) return ret;
    ret = require_gpio(rst); if (ret) return ret;
    ret = require_gpio(dfet); if (ret) return ret;

    /* Normal BQ operation: RST_SHUT low; DFETOFF kept inactive low. */
    ret = gpio_pin_configure_dt(rst, GPIO_OUTPUT_INACTIVE); if (ret) return ret;
    ret = gpio_pin_configure_dt(dfet, GPIO_OUTPUT_INACTIVE); if (ret) return ret;

    ret = gpio_pin_configure_dt(alert, GPIO_INPUT); if (ret) return ret;
    gpio_init_callback(cb, handler, BIT(alert->pin));
    ret = gpio_add_callback(alert->port, cb); if (ret) return ret;
    return gpio_pin_interrupt_configure_dt(alert, GPIO_INT_EDGE_TO_ACTIVE);
}

int bms_board_stm32_init(void)
{
    atomic_clear(&alert_count[0]);
    atomic_clear(&alert_count[1]);

    int ret = init_bq_gpio(&bq0_alert, &bq0_rst, &bq0_dfet,
                           &bq0_alert_cb, alert0_handler);
    if (ret) return ret;

#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
    ret = init_bq_gpio(&bq1_alert, &bq1_rst, &bq1_dfet,
                       &bq1_alert_cb, alert1_handler);
    if (ret) return ret;
#endif

    ret = require_gpio(&rs485_de); if (ret) return ret;
    ret = gpio_pin_configure_dt(&rs485_de, GPIO_OUTPUT_INACTIVE); if (ret) return ret;

#if DT_NODE_HAS_PROP(BOARD_IO_NODE, buzzer_gpios)
    ret = require_gpio(&buzzer); if (ret) return ret;
    ret = gpio_pin_configure_dt(&buzzer, GPIO_OUTPUT_INACTIVE); if (ret) return ret;
#endif
#if DT_NODE_HAS_PROP(BOARD_IO_NODE, start_gpios)
    ret = require_gpio(&start_input); if (ret) return ret;
    ret = gpio_pin_configure_dt(&start_input, GPIO_INPUT); if (ret) return ret;
#endif

    return 0;
}

uint32_t bms_board_stm32_alert_count(uint8_t monitor_index)
{
    if (monitor_index > 1U) return 0U;
    return (uint32_t)atomic_get(&alert_count[monitor_index]);
}

int bms_board_stm32_print_status(void)
{
    int a0 = gpio_pin_get_dt(&bq0_alert);
    if (a0 < 0) return a0;

    printk("BQ0 ALERT=%d irq=%u", a0, bms_board_stm32_alert_count(0));
#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
    int a1 = gpio_pin_get_dt(&bq1_alert);
    if (a1 < 0) return a1;
    printk(" | BQ1 ALERT=%d irq=%u", a1, bms_board_stm32_alert_count(1));
#endif
    printk("\n");
    return 0;
}
