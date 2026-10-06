#ifndef BMS_ESTIMATOR_H_
#define BMS_ESTIMATOR_H_

#include <stdbool.h>
#include <stdint.h>

#include "bms_snapshot.h"

/*
 * SOC units:
 *   centi-percent: 10000 = 100.00%, 5000 = 50.00%
 *
 * Capacity:
 *   mAh in configuration, uAh internally/counters.
 *
 * Current sign follows the project convention:
 *   positive = charging
 *   negative = discharging
 */
struct bms_estimator_config {
    uint32_t design_capacity_mAh;

    /*
     * Current estimate of usable full capacity.
     * Initially this can equal design capacity.
     * A future capacity-learning algorithm or persisted value may update it.
     */
    uint32_t initial_full_capacity_mAh;

    uint16_t initial_soc_centi_pct;

    /* Applied only while charging. 1000 = 100.0%. */
    uint16_t charge_efficiency_permille;

    /*
     * Do not integrate across unexpectedly large gaps.
     * A gap is re-anchored instead of inventing charge movement.
     */
    uint32_t max_integration_interval_ms;
};

struct bms_estimator_status {
    bool valid;

    uint32_t last_snapshot_sequence;
    int64_t last_update_ms;

    uint16_t soc_centi_pct;
    uint16_t soh_centi_pct;

    uint32_t design_capacity_mAh;
    uint32_t estimated_full_capacity_mAh;

    int64_t remaining_charge_uAh;

    uint64_t charged_uAh;
    uint64_t discharged_uAh;
    uint64_t throughput_uAh;

    uint64_t energy_in_uWh;
    uint64_t energy_out_uWh;

    /* Discharge-based equivalent full cycles, 1000 = 1.000 EFC. */
    uint32_t equivalent_full_cycles_milli;

    uint32_t skipped_intervals;
};


/*
 * Retained estimator state.
 *
 * This intentionally excludes timing anchors and fractional integration
 * remainders. After restore, the estimator re-anchors on the next snapshot.
 */
struct bms_estimator_persisted_state {
    int64_t remaining_charge_uAh;
    uint32_t estimated_full_capacity_mAh;

    uint64_t charged_uAh;
    uint64_t discharged_uAh;
    uint64_t throughput_uAh;

    uint64_t energy_in_uWh;
    uint64_t energy_out_uWh;

    uint32_t skipped_intervals;
};

struct bms_estimator {
    struct bms_estimator_config config;
    struct bms_estimator_status status;

    bool have_time_anchor;
    int64_t last_timestamp_ms;

    /*
     * Fractional remainders keep small integration steps from being lost.
     * Charge scaled denominator = 3,600,000:
     *   mA * ms * permille / 3,600,000 = uAh
     */
    int64_t charge_remainder_scaled;

    /* mA * ms / 3600 = uAh */
    uint64_t charge_in_remainder;
    uint64_t charge_out_remainder;
    uint64_t throughput_remainder;

    /* mV * mA * ms / 3,600,000 = uWh */
    uint64_t energy_in_remainder;
    uint64_t energy_out_remainder;
};

int bms_estimator_init(struct bms_estimator *,
                       const struct bms_estimator_config *);

/*
 * Consume one published snapshot.
 * Duplicate snapshot sequence numbers are ignored.
 */
int bms_estimator_process(struct bms_estimator *,
                          const struct bms_snapshot *,
                          struct bms_estimator_status *);

int bms_estimator_get_status(const struct bms_estimator *,
                             struct bms_estimator_status *);

/* Calibration / restore hooks; these do not read hardware. */
int bms_estimator_set_soc_centi_pct(struct bms_estimator *, uint16_t);
int bms_estimator_set_full_capacity_mAh(struct bms_estimator *, uint32_t);
int bms_estimator_reset_counters(struct bms_estimator *);

int bms_estimator_export_persisted_state(
    const struct bms_estimator *,
    struct bms_estimator_persisted_state *);

int bms_estimator_import_persisted_state(
    struct bms_estimator *,
    const struct bms_estimator_persisted_state *);

#endif
