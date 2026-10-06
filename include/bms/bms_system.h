#ifndef BMS_SYSTEM_H_
#define BMS_SYSTEM_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "bms_communications.h"
#include "bms_config_store.h"
#include "bms_system_policy.h"
#include "bms_watchdog.h"

enum bms_self_test_check {
    BMS_SELF_TEST_CONFIG       = (1U << 0),
    BMS_SELF_TEST_TOPOLOGY     = (1U << 1),
    BMS_SELF_TEST_MEASUREMENT  = (1U << 2),
    BMS_SELF_TEST_RUNTIME      = (1U << 3),
    BMS_SELF_TEST_COMMUNICATION= (1U << 4),
    BMS_SELF_TEST_STORE        = (1U << 5),
};

struct bms_system_config {
    uint32_t supervisor_period_ms;

    uint32_t max_snapshot_age_ms;
    uint32_t max_measurement_no_progress_ms;
    uint32_t max_runtime_no_progress_ms;
    uint32_t max_comm_no_progress_ms;
};

struct bms_system_health {
    enum bms_system_mode mode;
    uint64_t flags;

    bool self_test_passed;
    uint32_t self_test_failed_checks;

    bool safe_latched;
    bool maintenance_mode;

    int64_t snapshot_age_ms;

    uint32_t snapshot_sequence;
    uint32_t runtime_sequence;
    uint32_t can_publish_cycles;

    int measurement_error;
    int runtime_error;
    int communication_error;
    int watchdog_error;

    int64_t last_check_ms;
};

struct bms_system {
    struct bms_measurement *measurement;
    struct bms_core_runtime *runtime;
    struct bms_communications *communications;
    struct bms_topology *topology;
    struct bms_config_store *store;
    const struct bms_app_config *applied_config;

    const struct bms_watchdog_backend_ops *watchdog_ops;
    void *watchdog_context;

    struct bms_system_config config;

    struct k_mutex lock;
    struct k_work_delayable work;
    atomic_t running;

    bool initialized;
    bool self_test_passed;
    uint32_t self_test_failed_checks;

    bool safe_latched;
    bool maintenance_mode;

    enum bms_system_mode mode;
    uint64_t health_flags;

    uint32_t last_snapshot_sequence;
    int64_t last_snapshot_progress_ms;

    uint32_t last_runtime_sequence;
    int64_t last_runtime_progress_ms;

    uint32_t last_can_publish_cycles;
    int64_t last_comm_progress_ms;

    int watchdog_error;
};

int bms_system_init(
    struct bms_system *,
    struct bms_measurement *,
    struct bms_core_runtime *,
    struct bms_communications *,
    struct bms_topology *,
    struct bms_config_store *,
    const struct bms_app_config *,
    const struct bms_watchdog_backend_ops *,
    void *watchdog_context,
    const struct bms_system_config *);

int bms_system_run_self_test(struct bms_system *);

int bms_system_start(struct bms_system *);
int bms_system_stop(struct bms_system *);

int bms_system_check_now(struct bms_system *);

int bms_system_set_maintenance(struct bms_system *, bool enabled);

/*
 * SAFE_LATCHED never clears automatically.
 * Recovery is explicit and only succeeds when measurement/core can be
 * re-established.
 */
int bms_system_recover(struct bms_system *);

int bms_system_get_health(struct bms_system *,
                          struct bms_system_health *);

#endif
