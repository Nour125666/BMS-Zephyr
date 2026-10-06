#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "bms/bms_core_runtime.h"

static int process_once(struct bms_core_runtime *rt)
{
    if (rt == NULL || rt->measurement == NULL) {
        return -EINVAL;
    }

    struct bms_snapshot snapshot;
    int ret = bms_measurement_get_latest(rt->measurement, &snapshot);

    if (ret != 0) {
        k_mutex_lock(&rt->lock, K_FOREVER);
        rt->last_error = ret;
        k_mutex_unlock(&rt->lock);
        return ret;
    }

    int64_t age = bms_measurement_snapshot_age_ms(rt->measurement);

    struct bms_protection_decision protection;
    ret = bms_protection_evaluate(&rt->protection,
                                  &snapshot,
                                  age,
                                  &protection);
    if (ret != 0) {
        k_mutex_lock(&rt->lock, K_FOREVER);
        rt->last_error = ret;
        k_mutex_unlock(&rt->lock);
        return ret;
    }

    uint64_t previous_reasons = 0U;
    bool had_previous = false;

    k_mutex_lock(&rt->lock, K_FOREVER);
    if (rt->have_decision) {
        previous_reasons = rt->latest_decision.reasons;
        had_previous = true;
    }
    k_mutex_unlock(&rt->lock);

    if (!had_previous || previous_reasons != protection.reasons) {
        (void)bms_events_publish(&rt->events,
                                 BMS_EVENT_PROTECTION_CHANGED,
                                 protection.snapshot_sequence,
                                 protection.reasons,
                                 0);
    }

    /*
     * Estimation is another consumer of the SAME published snapshot.
     * It never reads hardware directly. Duplicate snapshot sequences are
     * ignored internally because runtime may tick faster than measurement.
     */
    struct bms_estimator_status estimator_status;

    k_mutex_lock(&rt->lock, K_FOREVER);
    ret = bms_estimator_process(&rt->estimator,
                                &snapshot,
                                &estimator_status);
    if (ret == 0) {
        rt->latest_estimator_status = estimator_status;
        rt->have_estimator_status = true;
    }
    k_mutex_unlock(&rt->lock);

    if (ret != 0) {
        k_mutex_lock(&rt->lock, K_FOREVER);
        rt->last_error = ret;
        k_mutex_unlock(&rt->lock);
        return ret;
    }

    /* State machine owns CHG/DSG switch/FET authority. */
    ret = bms_state_machine_update(&rt->state_machine, &protection);
    if (ret != 0) {
        k_mutex_lock(&rt->lock, K_FOREVER);
        rt->last_error = ret;
        k_mutex_unlock(&rt->lock);
        return ret;
    }

    /*
     * Balancing is a separate actuator policy. It consumes the SAME snapshot
     * and protection decision, then emits a desired GLOBAL cell mask.
     */
    struct bms_balancing_decision balancing;
    ret = bms_balancing_evaluate(&rt->balancing,
                                 &snapshot,
                                 &protection,
                                 &balancing);
    if (ret != 0) {
        k_mutex_lock(&rt->lock, K_FOREVER);
        rt->last_error = ret;
        k_mutex_unlock(&rt->lock);
        return ret;
    }

    uint64_t previous_applied;
    k_mutex_lock(&rt->lock, K_FOREVER);
    previous_applied = rt->applied_balance_mask;
    k_mutex_unlock(&rt->lock);

    if (balancing.desired_mask != previous_applied &&
        rt->balancing_actuation_enabled) {
        ret = bms_topology_set_balance_mask(rt->state_machine.topology,
                                            balancing.desired_mask);
        if (ret != 0) {
            (void)bms_events_publish(&rt->events,
                                     BMS_EVENT_ACTUATION_ERROR,
                                     balancing.snapshot_sequence,
                                     balancing.desired_mask,
                                     ret);

            k_mutex_lock(&rt->lock, K_FOREVER);
            rt->last_error = ret;
            k_mutex_unlock(&rt->lock);
            return ret;
        }

        (void)bms_events_publish(&rt->events,
                                 BMS_EVENT_BALANCE_CHANGED,
                                 balancing.snapshot_sequence,
                                 balancing.desired_mask,
                                 0);
    }

    k_mutex_lock(&rt->lock, K_FOREVER);
    rt->latest_decision = protection;
    rt->have_decision = true;
    rt->latest_balancing_decision = balancing;
    rt->have_balancing_decision = true;
    if (rt->balancing_actuation_enabled) {
        rt->applied_balance_mask = balancing.desired_mask;
    }
    rt->last_processed_sequence = snapshot.sequence;
    rt->last_error = 0;
    k_mutex_unlock(&rt->lock);

    return 0;
}

static void runtime_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);
    struct bms_core_runtime *rt =
        CONTAINER_OF(dwork, struct bms_core_runtime, work);

    if (atomic_get(&rt->running) == 0) {
        return;
    }

    (void)process_once(rt);

    if (atomic_get(&rt->running) != 0) {
        uint32_t period;

        k_mutex_lock(&rt->lock, K_FOREVER);
        period = rt->period_ms;
        k_mutex_unlock(&rt->lock);

        (void)k_work_schedule(&rt->work, K_MSEC(period));
    }
}

int bms_core_runtime_init(struct bms_core_runtime *rt,
                          struct bms_measurement *measurement,
                          struct bms_topology *topology,
                          const struct bms_protection_config *protection_config,
                          const struct bms_balancing_config *balancing_config,
                          const struct bms_estimator_config *estimator_config,
                          uint32_t period_ms,
                          bool actuation_enabled)
{
    if (rt == NULL || measurement == NULL || topology == NULL ||
        protection_config == NULL || balancing_config == NULL ||
        estimator_config == NULL || period_ms == 0U) {
        return -EINVAL;
    }

    memset(rt, 0, sizeof(*rt));

    rt->measurement = measurement;
    rt->period_ms = period_ms;
    rt->balancing_actuation_enabled = actuation_enabled;

    k_mutex_init(&rt->lock);
    k_work_init_delayable(&rt->work, runtime_work_handler);
    atomic_clear(&rt->running);

    int ret = bms_events_init(&rt->events);
    if (ret != 0) {
        return ret;
    }

    ret = bms_estimator_init(&rt->estimator, estimator_config);
    if (ret != 0) {
        return ret;
    }

    ret = bms_protection_init(&rt->protection, protection_config);
    if (ret != 0) {
        return ret;
    }

    ret = bms_balancing_init(&rt->balancing, balancing_config);
    if (ret != 0) {
        return ret;
    }

    ret = bms_state_machine_init(&rt->state_machine,
                                 topology,
                                 &rt->events,
                                 actuation_enabled);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

int bms_core_runtime_start(struct bms_core_runtime *rt)
{
    if (rt == NULL) return -EINVAL;
    if (!atomic_cas(&rt->running, 0, 1)) return 0;
    return k_work_schedule(&rt->work, K_NO_WAIT) < 0 ? -EIO : 0;
}

int bms_core_runtime_stop(struct bms_core_runtime *rt)
{
    if (rt == NULL) return -EINVAL;
    atomic_clear(&rt->running);
    (void)k_work_cancel_delayable(&rt->work);
    return 0;
}

int bms_core_runtime_process_now(struct bms_core_runtime *rt)
{
    return process_once(rt);
}

int bms_core_runtime_get_decision(struct bms_core_runtime *rt,
                                  struct bms_protection_decision *out)
{
    if (rt == NULL || out == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    if (!rt->have_decision) {
        k_mutex_unlock(&rt->lock);
        return -ENODATA;
    }
    *out = rt->latest_decision;
    k_mutex_unlock(&rt->lock);
    return 0;
}

int bms_core_runtime_get_estimator_status(
    struct bms_core_runtime *rt,
    struct bms_estimator_status *out)
{
    if (rt == NULL || out == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    if (!rt->have_estimator_status) {
        k_mutex_unlock(&rt->lock);
        return -ENODATA;
    }
    *out = rt->latest_estimator_status;
    k_mutex_unlock(&rt->lock);
    return 0;
}

int bms_core_runtime_set_soc_centi_pct(struct bms_core_runtime *rt,
                                       uint16_t soc_centi_pct)
{
    if (rt == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    int ret = bms_estimator_set_soc_centi_pct(&rt->estimator,
                                              soc_centi_pct);
    if (ret == 0) {
        (void)bms_estimator_get_status(&rt->estimator,
                                       &rt->latest_estimator_status);
        rt->have_estimator_status = true;
    }
    k_mutex_unlock(&rt->lock);
    return ret;
}

int bms_core_runtime_set_full_capacity_mAh(struct bms_core_runtime *rt,
                                           uint32_t full_capacity_mAh)
{
    if (rt == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    int ret = bms_estimator_set_full_capacity_mAh(&rt->estimator,
                                                  full_capacity_mAh);
    if (ret == 0) {
        (void)bms_estimator_get_status(&rt->estimator,
                                       &rt->latest_estimator_status);
        rt->have_estimator_status = true;
    }
    k_mutex_unlock(&rt->lock);
    return ret;
}

int bms_core_runtime_reset_estimator_counters(struct bms_core_runtime *rt)
{
    if (rt == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    int ret = bms_estimator_reset_counters(&rt->estimator);
    if (ret == 0) {
        (void)bms_estimator_get_status(&rt->estimator,
                                       &rt->latest_estimator_status);
        rt->have_estimator_status = true;
    }
    k_mutex_unlock(&rt->lock);
    return ret;
}

int bms_core_runtime_get_balancing_decision(
    struct bms_core_runtime *rt,
    struct bms_balancing_decision *out)
{
    if (rt == NULL || out == NULL) return -EINVAL;

    k_mutex_lock(&rt->lock, K_FOREVER);
    if (!rt->have_balancing_decision) {
        k_mutex_unlock(&rt->lock);
        return -ENODATA;
    }
    *out = rt->latest_balancing_decision;
    k_mutex_unlock(&rt->lock);
    return 0;
}

int bms_core_runtime_last_error(struct bms_core_runtime *rt)
{
    if (rt == NULL) return -EINVAL;
    k_mutex_lock(&rt->lock, K_FOREVER);
    int ret = rt->last_error;
    k_mutex_unlock(&rt->lock);
    return ret;
}

uint32_t bms_core_runtime_period_ms(struct bms_core_runtime *rt)
{
    if (rt == NULL) return 0U;
    k_mutex_lock(&rt->lock, K_FOREVER);
    uint32_t period = rt->period_ms;
    k_mutex_unlock(&rt->lock);
    return period;
}


int bms_core_runtime_export_estimator_persisted_state(
    struct bms_core_runtime *rt,
    struct bms_estimator_persisted_state *out)
{
    if (rt == NULL || out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&rt->lock, K_FOREVER);
    int ret = bms_estimator_export_persisted_state(&rt->estimator, out);
    k_mutex_unlock(&rt->lock);

    return ret;
}

int bms_core_runtime_import_estimator_persisted_state(
    struct bms_core_runtime *rt,
    const struct bms_estimator_persisted_state *in)
{
    if (rt == NULL || in == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&rt->lock, K_FOREVER);

    int ret = bms_estimator_import_persisted_state(&rt->estimator, in);

    if (ret == 0) {
        (void)bms_estimator_get_status(&rt->estimator,
                                       &rt->latest_estimator_status);
        rt->have_estimator_status = true;
    }

    k_mutex_unlock(&rt->lock);
    return ret;
}

int bms_core_runtime_apply_configs(
    struct bms_core_runtime *rt,
    const struct bms_protection_config *protection_config,
    const struct bms_balancing_config *balancing_config,
    const struct bms_estimator_config *estimator_config,
    bool preserve_estimator_state)
{
    if (rt == NULL || protection_config == NULL ||
        balancing_config == NULL || estimator_config == NULL) {
        return -EINVAL;
    }

    /*
     * Configuration changes are a controlled maintenance operation.
     * Do not rewrite policy structures while the periodic work item is active.
     */
    if (atomic_get(&rt->running) != 0) {
        return -EBUSY;
    }

    struct bms_estimator_persisted_state retained;
    bool retained_valid = false;

    k_mutex_lock(&rt->lock, K_FOREVER);

    if (preserve_estimator_state &&
        bms_estimator_export_persisted_state(&rt->estimator,
                                             &retained) == 0) {
        retained_valid = true;
    }

    int ret = bms_protection_init(&rt->protection, protection_config);

    if (ret == 0) {
        ret = bms_balancing_init(&rt->balancing, balancing_config);
    }

    if (ret == 0) {
        ret = bms_estimator_init(&rt->estimator, estimator_config);
    }

    if (ret == 0 && retained_valid) {
        ret = bms_estimator_import_persisted_state(&rt->estimator,
                                                   &retained);
    }

    if (ret == 0) {
        rt->have_decision = false;
        rt->have_balancing_decision = false;
        rt->applied_balance_mask = 0U;

        if (bms_estimator_get_status(&rt->estimator,
                                     &rt->latest_estimator_status) == 0) {
            rt->have_estimator_status = true;
        } else {
            rt->have_estimator_status = false;
        }
    }

    k_mutex_unlock(&rt->lock);
    return ret;
}

int bms_core_runtime_set_period(struct bms_core_runtime *rt,
                                uint32_t period_ms)
{
    if (rt == NULL || period_ms == 0U) {
        return -EINVAL;
    }

    k_mutex_lock(&rt->lock, K_FOREVER);
    rt->period_ms = period_ms;
    k_mutex_unlock(&rt->lock);

    return 0;
}


int bms_core_runtime_force_safe_outputs(struct bms_core_runtime *rt)
{
    if (rt == NULL || rt->state_machine.topology == NULL) {
        return -EINVAL;
    }

    struct bms_snapshot snapshot;
    uint32_t sequence = 0U;

    if (bms_measurement_get_latest(rt->measurement, &snapshot) == 0) {
        sequence = snapshot.sequence;
    }

    int balance_ret = bms_topology_set_balance_mask(
        rt->state_machine.topology, 0U);

    /*
     * Always attempt the all-off action even if clearing the balance mask
     * failed: forcing the switches off is the safety-critical output.
     */
    int off_ret = bms_state_machine_force_all_off(&rt->state_machine,
                                                  sequence);

    k_mutex_lock(&rt->lock, K_FOREVER);
    rt->applied_balance_mask = 0U;
    k_mutex_unlock(&rt->lock);

    return off_ret != 0 ? off_ret : balance_ret;
}
