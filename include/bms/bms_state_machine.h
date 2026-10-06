#ifndef BMS_STATE_MACHINE_H_
#define BMS_STATE_MACHINE_H_

#include <stdbool.h>

#include "bms_events.h"
#include "bms_protection.h"
#include "bms_topology.h"

enum bms_operating_state {
    BMS_STATE_INIT = 0,
    BMS_STATE_NORMAL,
    BMS_STATE_CHARGE_BLOCKED,
    BMS_STATE_DISCHARGE_BLOCKED,
    BMS_STATE_ALL_BLOCKED,
};

struct bms_state_machine {
    struct bms_topology *topology;
    struct bms_event_log *events;

    enum bms_operating_state state;
    bool actuation_enabled;
    int last_actuation_error;
};

int bms_state_machine_init(struct bms_state_machine *,
                           struct bms_topology *,
                           struct bms_event_log *,
                           bool actuation_enabled);

int bms_state_machine_update(struct bms_state_machine *,
                             const struct bms_protection_decision *);

/* Force physical outputs to the safe all-off state. */
int bms_state_machine_force_all_off(struct bms_state_machine *,
                                    uint32_t snapshot_sequence);

const char *bms_state_name(enum bms_operating_state state);

#endif
