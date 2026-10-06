#ifndef BMS_COMMUNICATIONS_H_
#define BMS_COMMUNICATIONS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "bms_config.h"
#include "bms_core_runtime.h"
#include "bms_measurement.h"
#include "bms_protocol.h"

struct bms_can_backend_ops {
    int (*send)(void *context, const struct bms_can_frame *);
};

struct bms_rs485_backend_ops {
    int (*send)(void *context, const uint8_t *data, size_t len);
};

struct bms_comm_config_ops {
    int (*get_applied)(void *context, struct bms_app_config *);
    int (*stage)(void *context, const struct bms_app_config *);
    int (*apply)(void *context);
    int (*save)(void *context);
};

struct bms_communications_stats {
    uint32_t can_publish_cycles;
    uint32_t can_frames_sent;
    uint32_t rs485_requests;
    uint32_t rs485_responses;
    uint32_t errors;

    uint32_t last_snapshot_sequence;
    int last_error;
};

struct bms_communications {
    struct bms_measurement *measurement;
    struct bms_core_runtime *runtime;

    const struct bms_can_backend_ops *can_ops;
    void *can_context;

    const struct bms_rs485_backend_ops *rs485_ops;
    void *rs485_context;

    const struct bms_comm_config_ops *config_ops;
    void *config_context;

    bool allow_remote_config_write;

    struct k_mutex lock;
    struct k_work_delayable work;

    atomic_t running;
    uint32_t can_period_ms;
    uint16_t rs485_tx_sequence;

    struct bms_communications_stats stats;
};

int bms_communications_init(
    struct bms_communications *,
    struct bms_measurement *,
    struct bms_core_runtime *,
    const struct bms_can_backend_ops *,
    void *can_context,
    const struct bms_rs485_backend_ops *,
    void *rs485_context,
    const struct bms_comm_config_ops *,
    void *config_context,
    bool allow_remote_config_write,
    uint32_t can_period_ms);

int bms_communications_start(struct bms_communications *);
int bms_communications_stop(struct bms_communications *);

int bms_communications_publish_can_now(struct bms_communications *);

int bms_communications_handle_rs485_request(
    struct bms_communications *,
    const uint8_t *request,
    size_t request_len);

int bms_communications_collect_view(
    struct bms_communications *,
    struct bms_telemetry_view *);

int bms_communications_get_stats(
    struct bms_communications *,
    struct bms_communications_stats *);

#endif
