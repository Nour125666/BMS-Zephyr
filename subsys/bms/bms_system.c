#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "bms/bms_system.h"

static void publish_mode_event(struct bms_system *sys,
                               enum bms_system_mode previous,
                               enum bms_system_mode next)
{
    if (previous == next) {
        return;
    }

    uint64_t data = ((uint64_t)(uint32_t)previous << 32) |
                    (uint32_t)next;

    (void)bms_events_publish(&sys->runtime->events,
                             BMS_EVENT_SYSTEM_MODE_CHANGED,
                             sys->last_snapshot_sequence,
                             data,
                             0);
}

static int enter_safe_latched(struct bms_system *sys,
                              uint64_t flags)
{
    if (sys->safe_latched) {
        return 0;
    }

    sys->safe_latched = true;

    /*
     * Stop the normal decision loop first so it cannot subsequently release
     * outputs while the supervisor is enforcing the safe state.
     */
    (void)bms_core_runtime_stop(sys->runtime);

    int ret = bms_core_runtime_force_safe_outputs(sys->runtime);

    (void)bms_events_publish(&sys->runtime->events,
                             BMS_EVENT_SYSTEM_SAFE_LATCHED,
                             sys->last_snapshot_sequence,
                             flags,
                             ret);

    return ret;
}

static uint64_t evaluate_health(struct bms_system *sys,
                                int64_t now_ms)
{
    uint64_t flags = 0U;

    if (!sys->self_test_passed) {
        flags |= BMS_HEALTH_SELF_TEST_FAILED;
    }

    bool maintenance = sys->maintenance_mode;

    if (!bms_measurement_has_snapshot(sys->measurement)) {
        flags |= BMS_HEALTH_NO_SNAPSHOT;
    } else {
        int64_t age =
            bms_measurement_snapshot_age_ms(sys->measurement);

        if (!maintenance &&
            (age < 0 ||
             age > (int64_t)sys->config.max_snapshot_age_ms)) {
            flags |= BMS_HEALTH_MEASUREMENT_STALE;
        }

        struct bms_snapshot snapshot;

        if (bms_measurement_get_latest(sys->measurement,
                                       &snapshot) == 0) {
            if (snapshot.sequence != sys->last_snapshot_sequence) {
                sys->last_snapshot_sequence = snapshot.sequence;
                sys->last_snapshot_progress_ms = now_ms;
            } else if (!maintenance &&
                       now_ms - sys->last_snapshot_progress_ms >
                       (int64_t)sys->config.max_measurement_no_progress_ms) {
                flags |= BMS_HEALTH_MEASUREMENT_NO_PROGRESS;
            }
        }
    }

    int measurement_error =
        bms_measurement_last_error(sys->measurement);

    if (!maintenance && measurement_error != 0) {
        flags |= BMS_HEALTH_MEASUREMENT_ERROR;
    }

    if (!maintenance &&
        atomic_get(&sys->measurement->running) == 0) {
        flags |= BMS_HEALTH_MEASUREMENT_STOPPED;
    }

    int runtime_error =
        bms_core_runtime_last_error(sys->runtime);

    if (!maintenance && runtime_error != 0) {
        flags |= BMS_HEALTH_RUNTIME_ERROR;
    }

    if (!maintenance && !sys->safe_latched &&
        atomic_get(&sys->runtime->running) == 0) {
        flags |= BMS_HEALTH_RUNTIME_STOPPED;
    }

    uint32_t runtime_sequence = sys->runtime->last_processed_sequence;

    if (runtime_sequence != sys->last_runtime_sequence) {
        sys->last_runtime_sequence = runtime_sequence;
        sys->last_runtime_progress_ms = now_ms;
    } else if (!maintenance && !sys->safe_latched &&
               now_ms - sys->last_runtime_progress_ms >
               (int64_t)sys->config.max_runtime_no_progress_ms) {
        flags |= BMS_HEALTH_RUNTIME_NO_PROGRESS;
    }

    struct bms_communications_stats comm_stats;
    int comm_ret =
        bms_communications_get_stats(sys->communications,
                                     &comm_stats);

    if (comm_ret != 0) {
        flags |= BMS_HEALTH_COMM_ERROR;
    } else {
        if (comm_stats.last_error != 0) {
            flags |= BMS_HEALTH_COMM_ERROR;
        }

        if (atomic_get(&sys->communications->running) == 0) {
            flags |= BMS_HEALTH_COMM_STOPPED;
        }

        if (comm_stats.can_publish_cycles !=
            sys->last_can_publish_cycles) {
            sys->last_can_publish_cycles =
                comm_stats.can_publish_cycles;
            sys->last_comm_progress_ms = now_ms;
        } else if (!maintenance &&
                   now_ms - sys->last_comm_progress_ms >
                   (int64_t)sys->config.max_comm_no_progress_ms) {
            flags |= BMS_HEALTH_COMM_NO_PROGRESS;
        }
    }

    return flags;
}

int bms_system_init(
    struct bms_system *sys,
    struct bms_measurement *measurement,
    struct bms_core_runtime *runtime,
    struct bms_communications *communications,
    struct bms_topology *topology,
    struct bms_config_store *store,
    const struct bms_app_config *applied_config,
    const struct bms_watchdog_backend_ops *watchdog_ops,
    void *watchdog_context,
    const struct bms_system_config *config)
{
    if (sys == NULL || measurement == NULL || runtime == NULL ||
        communications == NULL || topology == NULL ||
        applied_config == NULL || config == NULL ||
        config->supervisor_period_ms == 0U ||
        config->max_snapshot_age_ms == 0U ||
        config->max_measurement_no_progress_ms == 0U ||
        config->max_runtime_no_progress_ms == 0U ||
        config->max_comm_no_progress_ms == 0U) {
        return -EINVAL;
    }

    memset(sys, 0, sizeof(*sys));

    sys->measurement = measurement;
    sys->runtime = runtime;
    sys->communications = communications;
    sys->topology = topology;
    sys->store = store;
    sys->applied_config = applied_config;

    sys->watchdog_ops = watchdog_ops;
    sys->watchdog_context = watchdog_context;

    sys->config = *config;
    sys->mode = BMS_SYSTEM_BOOTING;

    k_mutex_init(&sys->lock);
    atomic_clear(&sys->running);

    int64_t now = k_uptime_get();
    sys->last_snapshot_progress_ms = now;
    sys->last_runtime_progress_ms = now;
    sys->last_comm_progress_ms = now;

    sys->initialized = true;
    return 0;
}

int bms_system_run_self_test(struct bms_system *sys)
{
    if (sys == NULL || !sys->initialized) {
        return -EINVAL;
    }

    uint32_t failed = 0U;

    if (bms_config_validate(sys->applied_config) != 0) {
        failed |= BMS_SELF_TEST_CONFIG;
    }

    if (sys->topology->config == NULL ||
        sys->topology->total_cell_count == 0U) {
        failed |= BMS_SELF_TEST_TOPOLOGY;
    }

    if (bms_measurement_refresh_now(sys->measurement) != 0) {
        failed |= BMS_SELF_TEST_MEASUREMENT;
    } else {
        struct bms_snapshot snapshot;

        if (bms_measurement_get_latest(sys->measurement,
                                       &snapshot) != 0 ||
            snapshot.cell_count !=
            sys->topology->total_cell_count) {
            failed |= BMS_SELF_TEST_MEASUREMENT;
        }
    }

    if (bms_core_runtime_process_now(sys->runtime) != 0) {
        failed |= BMS_SELF_TEST_RUNTIME;
    } else {
        struct bms_protection_decision decision;
        struct bms_estimator_status estimator;

        if (bms_core_runtime_get_decision(sys->runtime,
                                          &decision) != 0 ||
            bms_core_runtime_get_estimator_status(sys->runtime,
                                                  &estimator) != 0) {
            failed |= BMS_SELF_TEST_RUNTIME;
        }
    }

    struct bms_telemetry_view view;
    if (bms_communications_collect_view(sys->communications,
                                        &view) != 0) {
        failed |= BMS_SELF_TEST_COMMUNICATION;
    }

    if (sys->store != NULL) {
        struct bms_persistent_data saved;
        int ret = bms_config_store_load(sys->store, &saved);

        /*
         * Empty persistent storage is valid on a first boot.
         * Any other failure means a present/corrupt/incompatible image.
         */
        if (ret != 0 && ret != -ENOENT) {
            failed |= BMS_SELF_TEST_STORE;
        }
    }

    k_mutex_lock(&sys->lock, K_FOREVER);
    sys->self_test_failed_checks = failed;
    sys->self_test_passed = (failed == 0U);
    k_mutex_unlock(&sys->lock);

    (void)bms_events_publish(&sys->runtime->events,
                             BMS_EVENT_SELF_TEST,
                             sys->runtime->last_processed_sequence,
                             failed,
                             failed == 0U ? 0 : -EIO);

    if (failed != 0U) {
        (void)enter_safe_latched(sys,
                                 BMS_HEALTH_SELF_TEST_FAILED);
        return -EIO;
    }

    return 0;
}

int bms_system_check_now(struct bms_system *sys)
{
    if (sys == NULL || !sys->initialized) {
        return -EINVAL;
    }

    int64_t now = k_uptime_get();

    k_mutex_lock(&sys->lock, K_FOREVER);

    uint64_t flags = evaluate_health(sys, now);
    sys->health_flags = flags;

    enum bms_system_mode previous = sys->mode;
    enum bms_system_mode classified =
        bms_system_classify_health(flags,
                                   sys->self_test_passed,
                                   sys->maintenance_mode,
                                   sys->safe_latched);

    sys->mode = classified;

    k_mutex_unlock(&sys->lock);

    if (classified == BMS_SYSTEM_SAFE_LATCHED &&
        !sys->maintenance_mode) {
        (void)enter_safe_latched(sys, flags);
    }

    publish_mode_event(sys, previous, sys->mode);

    /*
     * Feed the platform watchdog only when the supervisor itself is alive
     * and the system is not in the fail-safe latch. DEGRADED communication
     * does not suppress feeding because core battery control is still healthy.
     */
    if (sys->watchdog_ops != NULL &&
        sys->watchdog_ops->feed != NULL &&
        sys->self_test_passed &&
        !sys->safe_latched) {
        int ret = sys->watchdog_ops->feed(sys->watchdog_context);

        k_mutex_lock(&sys->lock, K_FOREVER);
        sys->watchdog_error = ret;
        k_mutex_unlock(&sys->lock);
    }

    return 0;
}

static void system_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);
    struct bms_system *sys =
        CONTAINER_OF(dwork, struct bms_system, work);

    if (atomic_get(&sys->running) == 0) {
        return;
    }

    (void)bms_system_check_now(sys);

    if (atomic_get(&sys->running) != 0) {
        (void)k_work_schedule(&sys->work,
                              K_MSEC(sys->config.supervisor_period_ms));
    }
}

int bms_system_start(struct bms_system *sys)
{
    if (sys == NULL || !sys->initialized) {
        return -EINVAL;
    }

    if (!sys->self_test_passed) {
        return -EACCES;
    }

    if (!atomic_cas(&sys->running, 0, 1)) {
        return 0;
    }

    k_work_init_delayable(&sys->work, system_work_handler);

    return k_work_schedule(&sys->work, K_NO_WAIT) < 0 ?
           -EIO : 0;
}

int bms_system_stop(struct bms_system *sys)
{
    if (sys == NULL) {
        return -EINVAL;
    }

    atomic_clear(&sys->running);
    (void)k_work_cancel_delayable(&sys->work);
    return 0;
}

int bms_system_set_maintenance(struct bms_system *sys,
                               bool enabled)
{
    if (sys == NULL || !sys->initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&sys->lock, K_FOREVER);
    sys->maintenance_mode = enabled;

    enum bms_system_mode previous = sys->mode;

    if (enabled && !sys->safe_latched) {
        sys->mode = BMS_SYSTEM_MAINTENANCE;
    }

    k_mutex_unlock(&sys->lock);

    publish_mode_event(sys, previous, sys->mode);
    return 0;
}

int bms_system_recover(struct bms_system *sys)
{
    if (sys == NULL || !sys->initialized) {
        return -EINVAL;
    }

    if (!sys->safe_latched) {
        return 0;
    }

    /*
     * Recovery is explicit:
     *  1) make sure measurement can run and acquire a fresh snapshot,
     *  2) reprocess protection/state from that fresh state,
     *  3) restart periodic runtime,
     *  4) clear the system latch only after those operations succeed.
     */
    (void)bms_system_set_maintenance(sys, true);

    int ret = bms_measurement_start(sys->measurement);

    if (ret == 0) {
        ret = bms_measurement_refresh_now(sys->measurement);
    }

    if (ret == 0) {
        ret = bms_core_runtime_process_now(sys->runtime);
    }

    if (ret == 0) {
        ret = bms_core_runtime_start(sys->runtime);
    }

    if (ret == 0 &&
        atomic_get(&sys->communications->running) == 0) {
        ret = bms_communications_start(sys->communications);
    }

    if (ret == 0) {
        k_mutex_lock(&sys->lock, K_FOREVER);

        sys->safe_latched = false;
        sys->health_flags = 0U;

        int64_t now = k_uptime_get();
        sys->last_snapshot_progress_ms = now;
        sys->last_runtime_progress_ms = now;
        sys->last_comm_progress_ms = now;

        k_mutex_unlock(&sys->lock);

        (void)bms_system_set_maintenance(sys, false);
        (void)bms_system_check_now(sys);

        (void)bms_events_publish(&sys->runtime->events,
                                 BMS_EVENT_SYSTEM_RECOVERED,
                                 sys->runtime->last_processed_sequence,
                                 0U,
                                 0);
    } else {
        (void)bms_system_set_maintenance(sys, false);
    }

    return ret;
}

int bms_system_get_health(struct bms_system *sys,
                          struct bms_system_health *out)
{
    if (sys == NULL || out == NULL) {
        return -EINVAL;
    }

    struct bms_communications_stats comm_stats;
    memset(&comm_stats, 0, sizeof(comm_stats));
    (void)bms_communications_get_stats(sys->communications,
                                       &comm_stats);

    k_mutex_lock(&sys->lock, K_FOREVER);

    memset(out, 0, sizeof(*out));

    out->mode = sys->mode;
    out->flags = sys->health_flags;

    out->self_test_passed = sys->self_test_passed;
    out->self_test_failed_checks =
        sys->self_test_failed_checks;

    out->safe_latched = sys->safe_latched;
    out->maintenance_mode = sys->maintenance_mode;

    out->snapshot_age_ms =
        bms_measurement_snapshot_age_ms(sys->measurement);

    out->snapshot_sequence =
        sys->last_snapshot_sequence;
    out->runtime_sequence =
        sys->last_runtime_sequence;
    out->can_publish_cycles =
        sys->last_can_publish_cycles;

    out->measurement_error =
        bms_measurement_last_error(sys->measurement);
    out->runtime_error =
        bms_core_runtime_last_error(sys->runtime);
    out->communication_error =
        comm_stats.last_error;
    out->watchdog_error =
        sys->watchdog_error;

    out->last_check_ms = k_uptime_get();

    k_mutex_unlock(&sys->lock);
    return 0;
}
