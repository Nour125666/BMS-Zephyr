#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "bms_communications_stm32.h"

#define BOARD_IO_NODE DT_NODELABEL(bms_board_io)
#define RS485_UART_NODE DT_NODELABEL(uart4)

static int stm32_can_send(void *context, const struct bms_can_frame *src)
{
    struct bms_stm32_comm_backend *b = context;
    if (b == NULL || src == NULL || b->can_dev == NULL) return -EINVAL;

    struct can_frame f = {0};
    f.id = src->id;
    f.dlc = src->dlc;
    memcpy(f.data, src->data, src->dlc);
    return can_send(b->can_dev, &f, K_MSEC(20), NULL, NULL);
}

static int stm32_rs485_send(void *context, const uint8_t *data, size_t len)
{
    struct bms_stm32_comm_backend *b = context;
    if (b == NULL || data == NULL || len == 0U) return -EINVAL;

    k_mutex_lock(&b->tx_lock, K_FOREVER);
    int ret = gpio_pin_set_dt(&b->rs485_de, 1);
    if (ret == 0) {
        k_busy_wait(20);
        for (size_t i = 0; i < len; ++i) uart_poll_out(b->uart_dev, data[i]);

        /*
         * uart_poll_out() only guarantees the byte left the CPU/FIFO, not that
         * it has fully shifted out of the transceiver. Wait for the UART to
         * report completion (bounded fallback) before releasing DE/RE.
         */
        int64_t deadline = k_uptime_get() + 5;
        while (!uart_irq_tx_complete(b->uart_dev) &&
               k_uptime_get() < deadline) {
            k_busy_wait(10);
        }

        ret = gpio_pin_set_dt(&b->rs485_de, 0);
    }
    k_mutex_unlock(&b->tx_lock);
    return ret;
}

const struct bms_can_backend_ops bms_stm32_can_ops = { .send = stm32_can_send };
const struct bms_rs485_backend_ops bms_stm32_rs485_ops = { .send = stm32_rs485_send };

int bms_stm32_comm_backend_init(struct bms_stm32_comm_backend *b)
{
    if (b == NULL) return -EINVAL;
    memset(b, 0, sizeof(*b));

    b->can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
    b->uart_dev = DEVICE_DT_GET(RS485_UART_NODE);
    b->rs485_de = GPIO_DT_SPEC_GET(BOARD_IO_NODE, rs485_de_re_gpios);

    if (!device_is_ready(b->can_dev) || !device_is_ready(b->uart_dev) ||
        !gpio_is_ready_dt(&b->rs485_de)) return -ENODEV;

    int ret = gpio_pin_configure_dt(&b->rs485_de, GPIO_OUTPUT_INACTIVE);
    if (ret) return ret;

    k_mutex_init(&b->tx_lock);
    ret = can_start(b->can_dev);
    if (ret == -EALREADY) ret = 0;
    return ret;
}

static void parser_reset(struct bms_stm32_comm_backend *b)
{
    b->rx_len = 0U;
    b->rx_expected = 0U;
}

static int parser_push(struct bms_stm32_comm_backend *b,
                       struct bms_communications *communications,
                       uint8_t byte)
{
    if (b->rx_len == 0U) {
        if (byte != BMS_RS485_SOF0) return 0;
        b->rx_buf[b->rx_len++] = byte;
        return 0;
    }

    if (b->rx_len == 1U) {
        if (byte != BMS_RS485_SOF1) {
            parser_reset(b);
            if (byte == BMS_RS485_SOF0) b->rx_buf[b->rx_len++] = byte;
            return 0;
        }
        b->rx_buf[b->rx_len++] = byte;
        return 0;
    }

    if (b->rx_len >= sizeof(b->rx_buf)) {
        parser_reset(b);
        return -EMSGSIZE;
    }

    b->rx_buf[b->rx_len++] = byte;

    /* Header: SOF2 + ver1 + type1 + seq2 + len2 = 8 bytes. */
    if (b->rx_len == 8U) {
        uint16_t payload_len = (uint16_t)b->rx_buf[6] |
                               ((uint16_t)b->rx_buf[7] << 8);
        if (payload_len > BMS_RS485_MAX_PAYLOAD) {
            parser_reset(b);
            return -EMSGSIZE;
        }
        b->rx_expected = 10U + payload_len;
    }

    if (b->rx_expected != 0U && b->rx_len == b->rx_expected) {
        int ret = bms_communications_handle_rs485_request(
            communications, b->rx_buf, b->rx_len);
        parser_reset(b);
        return ret;
    }

    return 0;
}

int bms_stm32_comm_poll_rs485(struct bms_stm32_comm_backend *b,
                              struct bms_communications *communications)
{
    if (b == NULL || communications == NULL) return -EINVAL;

    uint8_t byte;
    int ret = 0;
    while (uart_poll_in(b->uart_dev, &byte) == 0) {
        int one = parser_push(b, communications, byte);
        if (one != 0) ret = one;
    }
    return ret;
}
