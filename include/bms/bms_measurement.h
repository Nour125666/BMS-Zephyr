#ifndef BMS_MEASUREMENT_H_
#define BMS_MEASUREMENT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "bms_topology.h"

struct bms_measurement {
    struct bms_topology *topology;

    struct k_mutex lock;
    struct k_work_delayable work;

    atomic_t running;
    atomic_t valid;

    uint32_t period_ms;
    int last_error;
    int64_t last_update_ms;

    struct bms_snapshot latest;
};

int bms_measurement_init(struct bms_measurement *,
                         struct bms_topology *,
                         uint32_t period_ms);

int bms_measurement_start(struct bms_measurement *);
int bms_measurement_stop(struct bms_measurement *);
int bms_measurement_set_period(struct bms_measurement *, uint32_t period_ms);

/* Perform one synchronous hardware/topology refresh and publish it. */
int bms_measurement_refresh_now(struct bms_measurement *);

/* Copy the most recently published coherent snapshot. No hardware read. */
int bms_measurement_get_latest(struct bms_measurement *,
                               struct bms_snapshot *);

bool bms_measurement_has_snapshot(const struct bms_measurement *);
int64_t bms_measurement_snapshot_age_ms(struct bms_measurement *);
int bms_measurement_last_error(struct bms_measurement *);
uint32_t bms_measurement_period_ms(struct bms_measurement *);

#endif
