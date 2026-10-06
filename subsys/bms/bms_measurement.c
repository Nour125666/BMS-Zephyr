#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "bms/bms_measurement.h"

static int publish_once(struct bms_measurement *m)
{
    struct bms_snapshot next;
    int ret;

    if (m == NULL || m->topology == NULL) {
        return -EINVAL;
    }

    /*
     * Do all potentially slow monitor/I2C work WITHOUT holding the snapshot
     * mutex. Readers can keep consuming the previous valid snapshot while the
     * next one is being acquired.
     */
    ret = bms_topology_read_snapshot(m->topology, &next);
    if (ret != 0) {
        k_mutex_lock(&m->lock, K_FOREVER);
        m->last_error = ret;
        k_mutex_unlock(&m->lock);
        return ret;
    }

    next.captured_at_ms = k_uptime_get();

    /* Atomic publication from the point of view of readers. */
    k_mutex_lock(&m->lock, K_FOREVER);
    m->latest = next;
    m->last_update_ms = next.captured_at_ms;
    m->last_error = 0;
    atomic_set(&m->valid, 1);
    k_mutex_unlock(&m->lock);

    return 0;
}

static void measurement_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);
    struct bms_measurement *m =
        CONTAINER_OF(dwork, struct bms_measurement, work);

    if (atomic_get(&m->running) == 0) {
        return;
    }

    (void)publish_once(m);

    if (atomic_get(&m->running) != 0) {
        uint32_t period;

        k_mutex_lock(&m->lock, K_FOREVER);
        period = m->period_ms;
        k_mutex_unlock(&m->lock);

        (void)k_work_schedule(&m->work, K_MSEC(period));
    }
}

int bms_measurement_init(struct bms_measurement *m,
                         struct bms_topology *topology,
                         uint32_t period_ms)
{
    if (m == NULL || topology == NULL || period_ms == 0U) {
        return -EINVAL;
    }

    memset(m, 0, sizeof(*m));
    m->topology = topology;
    m->period_ms = period_ms;

    k_mutex_init(&m->lock);
    k_work_init_delayable(&m->work, measurement_work_handler);
    atomic_clear(&m->running);
    atomic_clear(&m->valid);

    return 0;
}

int bms_measurement_start(struct bms_measurement *m)
{
    if (m == NULL || m->topology == NULL) {
        return -EINVAL;
    }

    if (!atomic_cas(&m->running, 0, 1)) {
        return 0;
    }

    return k_work_schedule(&m->work, K_NO_WAIT) < 0 ? -EIO : 0;
}

int bms_measurement_stop(struct bms_measurement *m)
{
    if (m == NULL) {
        return -EINVAL;
    }

    atomic_clear(&m->running);
    (void)k_work_cancel_delayable(&m->work);
    return 0;
}

int bms_measurement_set_period(struct bms_measurement *m, uint32_t period_ms)
{
    if (m == NULL || period_ms == 0U) {
        return -EINVAL;
    }

    k_mutex_lock(&m->lock, K_FOREVER);
    m->period_ms = period_ms;
    k_mutex_unlock(&m->lock);

    if (atomic_get(&m->running) != 0) {
        (void)k_work_reschedule(&m->work, K_MSEC(period_ms));
    }

    return 0;
}

int bms_measurement_refresh_now(struct bms_measurement *m)
{
    return publish_once(m);
}

int bms_measurement_get_latest(struct bms_measurement *m,
                               struct bms_snapshot *out)
{
    if (m == NULL || out == NULL) {
        return -EINVAL;
    }

    if (atomic_get(&m->valid) == 0) {
        return -ENODATA;
    }

    k_mutex_lock(&m->lock, K_FOREVER);
    *out = m->latest;
    k_mutex_unlock(&m->lock);

    return 0;
}

bool bms_measurement_has_snapshot(const struct bms_measurement *m)
{
    return m != NULL && atomic_get((atomic_t *)&m->valid) != 0;
}

int64_t bms_measurement_snapshot_age_ms(struct bms_measurement *m)
{
    if (m == NULL || atomic_get(&m->valid) == 0) {
        return -1;
    }

    k_mutex_lock(&m->lock, K_FOREVER);
    int64_t last = m->last_update_ms;
    k_mutex_unlock(&m->lock);

    return k_uptime_get() - last;
}

int bms_measurement_last_error(struct bms_measurement *m)
{
    if (m == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&m->lock, K_FOREVER);
    int ret = m->last_error;
    k_mutex_unlock(&m->lock);
    return ret;
}

uint32_t bms_measurement_period_ms(struct bms_measurement *m)
{
    if (m == NULL) {
        return 0U;
    }

    k_mutex_lock(&m->lock, K_FOREVER);
    uint32_t period = m->period_ms;
    k_mutex_unlock(&m->lock);
    return period;
}
