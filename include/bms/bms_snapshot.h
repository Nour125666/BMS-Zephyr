#ifndef BMS_SNAPSHOT_H_
#define BMS_SNAPSHOT_H_

#include <stdbool.h>
#include <stdint.h>
#include "bms_types.h"

struct bms_snapshot {
    uint32_t sequence;
    int64_t captured_at_ms; /* stamped by bms_measurement when published */
    uint8_t monitor_count;
    uint8_t cell_count;

    int32_t cell_mv[CONFIG_BMS_MAX_CELLS];
    int32_t cell_sum_mv;

    bool stack_voltage_valid;
    int32_t stack_mv;

    bool pack_voltage_valid;
    int32_t pack_mv;

    bool current_valid;
    int32_t current_ma;

    int32_t min_cell_mv;
    int32_t max_cell_mv;
    uint8_t min_cell_index;
    uint8_t max_cell_index;

    uint64_t alerts;
    uint64_t faults;
    bool permanent_fail;

    uint64_t balance_mask;

    bool switch_state_valid;
    struct bms_monitor_switch_state switch_state;
};

#endif
