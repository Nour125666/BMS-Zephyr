#include <errno.h>
#include <string.h>

#include "bms/bms_communications_virtual.h"

void bms_virtual_can_init(struct bms_virtual_can_backend *v)
{
    if (v != NULL) {
        memset(v, 0, sizeof(*v));
    }
}

void bms_virtual_rs485_init(struct bms_virtual_rs485_backend *v)
{
    if (v != NULL) {
        memset(v, 0, sizeof(*v));
    }
}

static int virtual_can_send(void *context,
                            const struct bms_can_frame *frame)
{
    struct bms_virtual_can_backend *v = context;

    if (v == NULL || frame == NULL) {
        return -EINVAL;
    }

    if (v->count < BMS_VIRTUAL_CAN_HISTORY) {
        v->frames[v->count++] = *frame;
    } else {
        memmove(&v->frames[0],
                &v->frames[1],
                sizeof(v->frames[0]) *
                (BMS_VIRTUAL_CAN_HISTORY - 1U));

        v->frames[BMS_VIRTUAL_CAN_HISTORY - 1U] = *frame;
    }

    v->total_sent++;
    return 0;
}

static int virtual_rs485_send(void *context,
                              const uint8_t *data,
                              size_t len)
{
    struct bms_virtual_rs485_backend *v = context;

    if (v == NULL || data == NULL ||
        len == 0U || len > sizeof(v->last_frame)) {
        return -EINVAL;
    }

    memcpy(v->last_frame, data, len);
    v->last_len = len;
    v->total_sent++;

    return 0;
}

const struct bms_can_backend_ops bms_virtual_can_ops = {
    .send = virtual_can_send,
};

const struct bms_rs485_backend_ops bms_virtual_rs485_ops = {
    .send = virtual_rs485_send,
};
