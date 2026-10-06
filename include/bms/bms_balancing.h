#ifndef BMS_BALANCING_H_
#define BMS_BALANCING_H_

#include <stdbool.h>
#include <stdint.h>

#include "bms_protection.h"
#include "bms_snapshot.h"

enum bms_balancing_inhibit_reason {
    BMS_BAL_INHIBIT_DISABLED        = (1U << 0),
    BMS_BAL_INHIBIT_PROTECTION      = (1U << 1),
    BMS_BAL_INHIBIT_CURRENT_INVALID = (1U << 2),
    BMS_BAL_INHIBIT_CURRENT_HIGH    = (1U << 3),
    BMS_BAL_INHIBIT_VOLTAGE_LOW     = (1U << 4),
    BMS_BAL_INHIBIT_DELTA_SMALL     = (1U << 5),
};

struct bms_balancing_config {
    bool enabled;

    /* Start only above this absolute cell voltage. */
    int32_t start_cell_mv;

    /* Start a new cell when it is this far above pack minimum. */
    int32_t start_delta_mv;

    /* Once active, keep it until delta falls to this value. */
    int32_t stop_delta_mv;

    /* Balancing is inhibited when |pack current| exceeds this value. */
    int32_t max_abs_current_ma;

    /* Thermal/power limit for simultaneous balancing cells. */
    uint8_t max_simultaneous_cells;
};

struct bms_balancing_decision {
    uint32_t snapshot_sequence;
    uint64_t eligible_mask;
    uint64_t desired_mask;
    uint32_t inhibit_reasons;
};

struct bms_balancing {
    struct bms_balancing_config config;

    /* Previous desired mask is used for per-cell hysteresis. */
    uint64_t previous_desired_mask;
    struct bms_balancing_decision latest;
};

int bms_balancing_init(struct bms_balancing *,
                       const struct bms_balancing_config *);

int bms_balancing_evaluate(struct bms_balancing *,
                           const struct bms_snapshot *,
                           const struct bms_protection_decision *,
                           struct bms_balancing_decision *);

int bms_balancing_set_enabled(struct bms_balancing *, bool enabled);
const char *bms_balancing_inhibit_name(uint32_t one_reason_bit);

#endif
