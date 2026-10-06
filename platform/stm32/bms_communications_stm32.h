#ifndef BMS_COMMUNICATIONS_STM32_H_
#define BMS_COMMUNICATIONS_STM32_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "bms/bms_communications.h"

struct bms_stm32_comm_backend {
    const struct device *can_dev;
    const struct device *uart_dev;
    struct gpio_dt_spec rs485_de;
    struct k_mutex tx_lock;

    uint8_t rx_buf[BMS_RS485_MAX_FRAME_SIZE];
    size_t rx_len;
    size_t rx_expected;
};

int bms_stm32_comm_backend_init(struct bms_stm32_comm_backend *);
int bms_stm32_comm_poll_rs485(struct bms_stm32_comm_backend *,
                              struct bms_communications *);

extern const struct bms_can_backend_ops bms_stm32_can_ops;
extern const struct bms_rs485_backend_ops bms_stm32_rs485_ops;

#endif
