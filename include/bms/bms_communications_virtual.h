#ifndef BMS_COMMUNICATIONS_VIRTUAL_H_
#define BMS_COMMUNICATIONS_VIRTUAL_H_

#include <stddef.h>
#include <stdint.h>

#include "bms_communications.h"

#define BMS_VIRTUAL_CAN_HISTORY 32U

struct bms_virtual_can_backend {
    struct bms_can_frame frames[BMS_VIRTUAL_CAN_HISTORY];
    size_t count;
    uint32_t total_sent;
};

struct bms_virtual_rs485_backend {
    uint8_t last_frame[BMS_RS485_MAX_FRAME_SIZE];
    size_t last_len;
    uint32_t total_sent;
};

void bms_virtual_can_init(struct bms_virtual_can_backend *);
void bms_virtual_rs485_init(struct bms_virtual_rs485_backend *);

extern const struct bms_can_backend_ops bms_virtual_can_ops;
extern const struct bms_rs485_backend_ops bms_virtual_rs485_ops;

#endif
