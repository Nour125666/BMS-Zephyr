#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "bms/bms_events.h"

int bms_events_init(struct bms_event_log *log)
{
    if (log == NULL) {
        return -EINVAL;
    }

    memset(log, 0, sizeof(*log));
    k_mutex_init(&log->lock);
    log->next_id = 1U;
    return 0;
}

int bms_events_publish(struct bms_event_log *log,
                       enum bms_event_type type,
                       uint32_t snapshot_sequence,
                       uint64_t data,
                       int32_t detail)
{
    if (log == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&log->lock, K_FOREVER);

    struct bms_event_record *r = &log->records[log->head];
    r->id = log->next_id++;
    r->timestamp_ms = k_uptime_get();
    r->type = type;
    r->snapshot_sequence = snapshot_sequence;
    r->data = data;
    r->detail = detail;

    log->head = (uint16_t)((log->head + 1U) % CONFIG_BMS_EVENT_LOG_CAPACITY);
    if (log->count < CONFIG_BMS_EVENT_LOG_CAPACITY) {
        log->count++;
    }

    k_mutex_unlock(&log->lock);
    return 0;
}

int bms_events_copy(struct bms_event_log *log,
                    struct bms_event_record *out,
                    size_t max_records,
                    size_t *out_count)
{
    if (log == NULL || out_count == NULL ||
        (max_records > 0U && out == NULL)) {
        return -EINVAL;
    }

    k_mutex_lock(&log->lock, K_FOREVER);

    size_t count = log->count;
    if (count > max_records) {
        count = max_records;
    }

    /*
     * Oldest stored record:
     * head points at the next write position.
     */
    uint16_t oldest =
        (uint16_t)((log->head + CONFIG_BMS_EVENT_LOG_CAPACITY - log->count) %
                   CONFIG_BMS_EVENT_LOG_CAPACITY);

    /* If truncating, skip older records and retain the newest ones. */
    size_t skip = (size_t)log->count - count;

    for (size_t i = 0U; i < count; ++i) {
        uint16_t idx =
            (uint16_t)((oldest + skip + i) % CONFIG_BMS_EVENT_LOG_CAPACITY);
        out[i] = log->records[idx];
    }

    *out_count = count;

    k_mutex_unlock(&log->lock);
    return 0;
}

int bms_events_clear(struct bms_event_log *log)
{
    if (log == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&log->lock, K_FOREVER);
    memset(log->records, 0, sizeof(log->records));
    log->head = 0U;
    log->count = 0U;
    k_mutex_unlock(&log->lock);

    return 0;
}

const char *bms_event_type_name(enum bms_event_type type)
{
    switch (type) {
    case BMS_EVENT_PROTECTION_CHANGED:
        return "PROTECTION_CHANGED";
    case BMS_EVENT_STATE_CHANGED:
        return "STATE_CHANGED";
    case BMS_EVENT_ACTUATION_ERROR:
        return "ACTUATION_ERROR";
    case BMS_EVENT_BALANCE_CHANGED:
        return "BALANCE_CHANGED";
    case BMS_EVENT_SELF_TEST:
        return "SELF_TEST";
    case BMS_EVENT_SYSTEM_MODE_CHANGED:
        return "SYSTEM_MODE_CHANGED";
    case BMS_EVENT_SYSTEM_SAFE_LATCHED:
        return "SYSTEM_SAFE_LATCHED";
    case BMS_EVENT_SYSTEM_RECOVERED:
        return "SYSTEM_RECOVERED";
    default:
        return "UNKNOWN";
    }
}
