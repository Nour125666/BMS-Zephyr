#include <errno.h>

#include "bms/bms_state_machine.h"

static enum bms_operating_state target_from_decision(
    const struct bms_protection_decision *d)
{
    if (d->charge_allowed && d->discharge_allowed) {
        return BMS_STATE_NORMAL;
    }
    if (!d->charge_allowed && d->discharge_allowed) {
        return BMS_STATE_CHARGE_BLOCKED;
    }
    if (d->charge_allowed && !d->discharge_allowed) {
        return BMS_STATE_DISCHARGE_BLOCKED;
    }
    return BMS_STATE_ALL_BLOCKED;
}

static enum bms_monitor_switch_request request_from_state(
    enum bms_operating_state state)
{
    switch (state) {
    case BMS_STATE_NORMAL:
        return BMS_SWITCH_RELEASE;
    case BMS_STATE_CHARGE_BLOCKED:
        return BMS_SWITCH_CHARGE_OFF;
    case BMS_STATE_DISCHARGE_BLOCKED:
        return BMS_SWITCH_DISCHARGE_OFF;
    case BMS_STATE_ALL_BLOCKED:
    case BMS_STATE_INIT:
    default:
        return BMS_SWITCH_ALL_OFF;
    }
}

int bms_state_machine_init(struct bms_state_machine *sm,
                           struct bms_topology *topology,
                           struct bms_event_log *events,
                           bool actuation_enabled)
{
    if (sm == NULL || topology == NULL || events == NULL) {
        return -EINVAL;
    }

    sm->topology = topology;
    sm->events = events;
    sm->state = BMS_STATE_INIT;
    sm->actuation_enabled = actuation_enabled;
    sm->last_actuation_error = 0;
    return 0;
}

int bms_state_machine_update(struct bms_state_machine *sm,
                             const struct bms_protection_decision *decision)
{
    if (sm == NULL || decision == NULL) {
        return -EINVAL;
    }

    enum bms_operating_state target = target_from_decision(decision);

    if (target == sm->state) {
        return 0;
    }

    int ret = 0;

    if (sm->actuation_enabled) {
        /*
         * This sends a hardware-neutral switch request through topology.
         *
         * Exact transition timing/atomicity of partial CHG/DSG release must
         * still be validated on the physical BQ hardware before production.
         */
        ret = bms_topology_set_switch(sm->topology,
                                      request_from_state(target));

        if (ret != 0) {
            sm->last_actuation_error = ret;
            (void)bms_events_publish(sm->events,
                                     BMS_EVENT_ACTUATION_ERROR,
                                     decision->snapshot_sequence,
                                     (uint64_t)target,
                                     ret);
            return ret;
        }
    }

    enum bms_operating_state previous = sm->state;
    sm->state = target;
    sm->last_actuation_error = 0;

    /*
     * data packs previous state in the high 32 bits and new state in the low
     * 32 bits for diagnostics.
     */
    uint64_t data = ((uint64_t)(uint32_t)previous << 32) |
                    (uint32_t)target;

    (void)bms_events_publish(sm->events,
                             BMS_EVENT_STATE_CHANGED,
                             decision->snapshot_sequence,
                             data,
                             0);

    return 0;
}

int bms_state_machine_force_all_off(struct bms_state_machine *sm,
                                    uint32_t snapshot_sequence)
{
    if (sm == NULL) {
        return -EINVAL;
    }

    int ret = 0;

    if (sm->actuation_enabled) {
        ret = bms_topology_set_switch(sm->topology,
                                      BMS_SWITCH_ALL_OFF);
        if (ret != 0) {
            sm->last_actuation_error = ret;

            (void)bms_events_publish(sm->events,
                                     BMS_EVENT_ACTUATION_ERROR,
                                     snapshot_sequence,
                                     (uint64_t)BMS_STATE_ALL_BLOCKED,
                                     ret);
            return ret;
        }
    }

    enum bms_operating_state previous = sm->state;
    sm->state = BMS_STATE_ALL_BLOCKED;
    sm->last_actuation_error = 0;

    if (previous != sm->state) {
        uint64_t data = ((uint64_t)(uint32_t)previous << 32) |
                        (uint32_t)sm->state;

        (void)bms_events_publish(sm->events,
                                 BMS_EVENT_STATE_CHANGED,
                                 snapshot_sequence,
                                 data,
                                 0);
    }

    return 0;
}

const char *bms_state_name(enum bms_operating_state state)
{
    switch (state) {
    case BMS_STATE_INIT:              return "INIT";
    case BMS_STATE_NORMAL:            return "NORMAL";
    case BMS_STATE_CHARGE_BLOCKED:    return "CHARGE_BLOCKED";
    case BMS_STATE_DISCHARGE_BLOCKED: return "DISCHARGE_BLOCKED";
    case BMS_STATE_ALL_BLOCKED:       return "ALL_BLOCKED";
    default:                          return "UNKNOWN";
    }
}
