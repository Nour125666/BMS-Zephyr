#ifndef BMS_EVENTS_H_
#define BMS_EVENTS_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifndef CONFIG_BMS_EVENT_LOG_CAPACITY
#define CONFIG_BMS_EVENT_LOG_CAPACITY 32
#endif

enum bms_event_type {
    BMS_EVENT_PROTECTION_CHANGED = 1,
    BMS_EVENT_STATE_CHANGED,
    BMS_EVENT_ACTUATION_ERROR,
    BMS_EVENT_BALANCE_CHANGED,
    BMS_EVENT_SELF_TEST,
    BMS_EVENT_SYSTEM_MODE_CHANGED,
    BMS_EVENT_SYSTEM_SAFE_LATCHED,
    BMS_EVENT_SYSTEM_RECOVERED,
};

struct bms_event_record {
    uint32_t id;
    int64_t timestamp_ms;
    enum bms_event_type type;
    uint32_t snapshot_sequence;

    /* Event-specific payload. */
    uint64_t data;
    int32_t detail;
};

struct bms_event_log {
    struct k_mutex lock;
    struct bms_event_record records[CONFIG_BMS_EVENT_LOG_CAPACITY];
    uint16_t head;
    uint16_t count;
    uint32_t next_id;
};

int bms_events_init(struct bms_event_log *);

int bms_events_publish(struct bms_event_log *,
                       enum bms_event_type type,
                       uint32_t snapshot_sequence,
                       uint64_t data,
                       int32_t detail);

/*
 * Copy records oldest -> newest.
 * If max_records is smaller than the stored count, the newest records are kept.
 */
int bms_events_copy(struct bms_event_log *,
                    struct bms_event_record *out,
                    size_t max_records,
                    size_t *out_count);

int bms_events_clear(struct bms_event_log *);

const char *bms_event_type_name(enum bms_event_type type);

#endif
