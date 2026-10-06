#ifndef BMS_MONITOR_H_
#define BMS_MONITOR_H_

#include <stdbool.h>
#include <stdint.h>
#include "bms_types.h"

enum bms_monitor_capability {
    BMS_MON_CAP_CELL_VOLTAGE     = (1U << 0),
    BMS_MON_CAP_STACK_VOLTAGE    = (1U << 1),
    BMS_MON_CAP_PACK_VOLTAGE     = (1U << 2),
    BMS_MON_CAP_CURRENT          = (1U << 3),
    BMS_MON_CAP_LOCAL_PROTECTION = (1U << 4),
    BMS_MON_CAP_BALANCING        = (1U << 5),
    BMS_MON_CAP_SWITCH_CONTROL   = (1U << 6),
};

struct bms_monitor;

struct bms_monitor_ops {
    int (*read_sample)(struct bms_monitor *, struct bms_monitor_sample *);
    int (*read_faults)(struct bms_monitor *, struct bms_monitor_fault_state *);
    int (*read_switch_state)(struct bms_monitor *, struct bms_monitor_switch_state *);
    int (*set_switch)(struct bms_monitor *, enum bms_monitor_switch_request);
    int (*get_balance_mask)(struct bms_monitor *, uint64_t *);
    int (*set_balance_mask)(struct bms_monitor *, uint64_t);
};

struct bms_monitor {
    const char *name;
    const struct bms_monitor_ops *ops;
    void *backend;
    uint32_t capabilities;
    uint8_t cell_count;
};

bool bms_monitor_has_capability(const struct bms_monitor *, enum bms_monitor_capability);
int bms_monitor_read_sample(struct bms_monitor *, struct bms_monitor_sample *);
int bms_monitor_read_faults(struct bms_monitor *, struct bms_monitor_fault_state *);
int bms_monitor_read_switch_state(struct bms_monitor *, struct bms_monitor_switch_state *);
int bms_monitor_set_switch(struct bms_monitor *, enum bms_monitor_switch_request);
int bms_monitor_get_balance_mask(struct bms_monitor *, uint64_t *);
int bms_monitor_set_balance_mask(struct bms_monitor *, uint64_t);

#endif
