#ifndef BMS_TYPES_H_
#define BMS_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef CONFIG_BMS_MONITOR_MAX_CELLS
#define CONFIG_BMS_MONITOR_MAX_CELLS 16
#endif
#ifndef CONFIG_BMS_MAX_CELLS
#define CONFIG_BMS_MAX_CELLS 32
#endif

enum bms_fault_flag {
    BMS_FAULT_CELL_UNDERVOLTAGE       = (1ULL << 0),
    BMS_FAULT_CELL_OVERVOLTAGE        = (1ULL << 1),
    BMS_FAULT_OVERCURRENT_CHARGE      = (1ULL << 2),
    BMS_FAULT_OVERCURRENT_DISCHARGE   = (1ULL << 3),
    BMS_FAULT_SHORT_CIRCUIT_DISCHARGE = (1ULL << 4),
    BMS_FAULT_PERMANENT_FAIL          = (1ULL << 24),
};

struct bms_monitor_sample {
    uint8_t cell_count;
    int32_t cell_mv[CONFIG_BMS_MONITOR_MAX_CELLS];

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
};

struct bms_monitor_fault_state {
    uint64_t alerts;
    uint64_t faults;
    bool permanent_fail;
};

struct bms_monitor_switch_state {
    bool charge_enabled;
    bool discharge_enabled;
    bool precharge_enabled;
    bool predischarge_enabled;
};

enum bms_monitor_switch_request {
    BMS_SWITCH_RELEASE = 0,
    BMS_SWITCH_CHARGE_OFF,
    BMS_SWITCH_DISCHARGE_OFF,
    BMS_SWITCH_ALL_OFF,
};

#endif
