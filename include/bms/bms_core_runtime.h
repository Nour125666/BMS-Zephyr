#ifndef BMS_CORE_RUNTIME_H_
#define BMS_CORE_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "bms_balancing.h"
#include "bms_events.h"
#include "bms_estimator.h"
#include "bms_measurement.h"
#include "bms_protection.h"
#include "bms_state_machine.h"

struct bms_core_runtime {
    struct bms_measurement *measurement;

    struct bms_event_log events;
    struct bms_estimator estimator;
    struct bms_protection protection;
    struct bms_balancing balancing;
    struct bms_state_machine state_machine;

    struct k_mutex lock;
    struct k_work_delayable work;

    atomic_t running;
    uint32_t period_ms;
    uint32_t last_processed_sequence;
    int last_error;

    bool have_decision;
    struct bms_protection_decision latest_decision;

    bool have_estimator_status;
    struct bms_estimator_status latest_estimator_status;

    bool have_balancing_decision;
    struct bms_balancing_decision latest_balancing_decision;
    uint64_t applied_balance_mask;
    bool balancing_actuation_enabled;
};

int bms_core_runtime_init(struct bms_core_runtime *,
                          struct bms_measurement *,
                          struct bms_topology *,
                          const struct bms_protection_config *,
                          const struct bms_balancing_config *,
                          const struct bms_estimator_config *,
                          uint32_t period_ms,
                          bool actuation_enabled);

int bms_core_runtime_start(struct bms_core_runtime *);
int bms_core_runtime_stop(struct bms_core_runtime *);
int bms_core_runtime_process_now(struct bms_core_runtime *);

int bms_core_runtime_get_decision(struct bms_core_runtime *,
                                  struct bms_protection_decision *);

int bms_core_runtime_get_estimator_status(
    struct bms_core_runtime *,
    struct bms_estimator_status *);

int bms_core_runtime_set_soc_centi_pct(struct bms_core_runtime *, uint16_t);
int bms_core_runtime_set_full_capacity_mAh(struct bms_core_runtime *, uint32_t);
int bms_core_runtime_reset_estimator_counters(struct bms_core_runtime *);

int bms_core_runtime_export_estimator_persisted_state(
    struct bms_core_runtime *,
    struct bms_estimator_persisted_state *);

int bms_core_runtime_import_estimator_persisted_state(
    struct bms_core_runtime *,
    const struct bms_estimator_persisted_state *);

int bms_core_runtime_apply_configs(
    struct bms_core_runtime *,
    const struct bms_protection_config *,
    const struct bms_balancing_config *,
    const struct bms_estimator_config *,
    bool preserve_estimator_state);

int bms_core_runtime_set_period(struct bms_core_runtime *, uint32_t);

/* System-supervisor fail-safe output override. */
int bms_core_runtime_force_safe_outputs(struct bms_core_runtime *);

int bms_core_runtime_get_balancing_decision(
    struct bms_core_runtime *,
    struct bms_balancing_decision *);

int bms_core_runtime_last_error(struct bms_core_runtime *);
uint32_t bms_core_runtime_period_ms(struct bms_core_runtime *);

#endif
