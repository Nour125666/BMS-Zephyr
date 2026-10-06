#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "bms/bms_communications.h"

static void record_error(struct bms_communications *c, int ret)
{
    k_mutex_lock(&c->lock, K_FOREVER);
    c->stats.errors++;
    c->stats.last_error = ret;
    k_mutex_unlock(&c->lock);
}

int bms_communications_collect_view(
    struct bms_communications *c,
    struct bms_telemetry_view *view)
{
    if (c == NULL || view == NULL) {
        return -EINVAL;
    }

    struct bms_snapshot snapshot;
    int ret = bms_measurement_get_latest(c->measurement, &snapshot);
    if (ret != 0) {
        return ret;
    }

    struct bms_protection_decision protection;
    ret = bms_core_runtime_get_decision(c->runtime, &protection);
    if (ret != 0) {
        return ret;
    }

    struct bms_estimator_status estimator;
    ret = bms_core_runtime_get_estimator_status(c->runtime, &estimator);
    if (ret != 0) {
        return ret;
    }

    memset(view, 0, sizeof(*view));

    view->snapshot_sequence = snapshot.sequence;
    view->captured_at_ms = snapshot.captured_at_ms;

    view->cell_count = snapshot.cell_count;
    memcpy(view->cell_mv,
           snapshot.cell_mv,
           sizeof(view->cell_mv));

    view->pack_mv = snapshot.pack_mv;
    view->current_ma = snapshot.current_ma;

    view->min_cell_mv = snapshot.min_cell_mv;
    view->max_cell_mv = snapshot.max_cell_mv;
    view->min_cell_index = snapshot.min_cell_index;
    view->max_cell_index = snapshot.max_cell_index;

    view->faults = snapshot.faults;
    view->balance_mask = snapshot.balance_mask;

    view->state = c->runtime->state_machine.state;

    view->charge_allowed = protection.charge_allowed;
    view->discharge_allowed = protection.discharge_allowed;

    if (snapshot.switch_state_valid) {
        view->charge_switch_on =
            snapshot.switch_state.charge_enabled;
        view->discharge_switch_on =
            snapshot.switch_state.discharge_enabled;
    }

    view->soc_centi_pct = estimator.soc_centi_pct;
    view->soh_centi_pct = estimator.soh_centi_pct;
    view->estimated_full_capacity_mAh =
        estimator.estimated_full_capacity_mAh;
    view->equivalent_full_cycles_milli =
        estimator.equivalent_full_cycles_milli;

    return 0;
}

int bms_communications_publish_can_now(struct bms_communications *c)
{
    if (c == NULL || c->can_ops == NULL ||
        c->can_ops->send == NULL) {
        return -EINVAL;
    }

    struct bms_telemetry_view view;
    int ret = bms_communications_collect_view(c, &view);
    if (ret != 0) {
        record_error(c, ret);
        return ret;
    }

    struct bms_can_frame frames[BMS_CAN_MAX_FRAMES];
    size_t count = 0U;

    ret = bms_protocol_build_can_frames(&view,
                                        frames,
                                        BMS_CAN_MAX_FRAMES,
                                        &count);
    if (ret != 0) {
        record_error(c, ret);
        return ret;
    }

    for (size_t i = 0U; i < count; ++i) {
        ret = c->can_ops->send(c->can_context, &frames[i]);
        if (ret != 0) {
            record_error(c, ret);
            return ret;
        }
    }

    k_mutex_lock(&c->lock, K_FOREVER);
    c->stats.can_publish_cycles++;
    c->stats.can_frames_sent += (uint32_t)count;
    c->stats.last_snapshot_sequence = view.snapshot_sequence;
    c->stats.last_error = 0;
    k_mutex_unlock(&c->lock);

    return 0;
}

static int send_rs485_frame(struct bms_communications *c,
                            const struct bms_rs485_frame *frame)
{
    if (c->rs485_ops == NULL || c->rs485_ops->send == NULL) {
        return -ENOTSUP;
    }

    uint8_t bytes[BMS_RS485_MAX_FRAME_SIZE];
    size_t len = 0U;

    int ret = bms_protocol_rs485_encode(frame,
                                        bytes,
                                        sizeof(bytes),
                                        &len);
    if (ret != 0) {
        return ret;
    }

    ret = c->rs485_ops->send(c->rs485_context, bytes, len);

    if (ret == 0) {
        k_mutex_lock(&c->lock, K_FOREVER);
        c->stats.rs485_responses++;
        c->stats.last_error = 0;
        k_mutex_unlock(&c->lock);
    }

    return ret;
}

static int send_result(struct bms_communications *c,
                       uint16_t sequence,
                       int result)
{
    struct bms_rs485_frame response = {
        .type = result == 0 ?
                BMS_RS485_RSP_ACK :
                BMS_RS485_RSP_ERROR,
        .sequence = sequence,
        .payload_len = 4U,
    };

    response.payload[0] = (uint8_t)((uint32_t)result & 0xFFU);
    response.payload[1] = (uint8_t)(((uint32_t)result >> 8) & 0xFFU);
    response.payload[2] = (uint8_t)(((uint32_t)result >> 16) & 0xFFU);
    response.payload[3] = (uint8_t)(((uint32_t)result >> 24) & 0xFFU);

    return send_rs485_frame(c, &response);
}

int bms_communications_handle_rs485_request(
    struct bms_communications *c,
    const uint8_t *request_bytes,
    size_t request_len)
{
    if (c == NULL || request_bytes == NULL ||
        request_len == 0U) {
        return -EINVAL;
    }

    struct bms_rs485_frame request;
    int ret = bms_protocol_rs485_decode(request_bytes,
                                        request_len,
                                        &request);
    if (ret != 0) {
        record_error(c, ret);
        return ret;
    }

    k_mutex_lock(&c->lock, K_FOREVER);
    c->stats.rs485_requests++;
    k_mutex_unlock(&c->lock);

    struct bms_rs485_frame response;
    memset(&response, 0, sizeof(response));
    response.sequence = request.sequence;

    switch (request.type) {
    case BMS_RS485_REQ_STATUS: {
        struct bms_telemetry_view view;

        ret = bms_communications_collect_view(c, &view);
        if (ret != 0) {
            break;
        }

        response.type = BMS_RS485_RSP_STATUS;

        size_t len = 0U;
        ret = bms_protocol_encode_status_payload(
            &view,
            response.payload,
            sizeof(response.payload),
            &len);

        if (ret == 0) {
            response.payload_len = (uint16_t)len;
            ret = send_rs485_frame(c, &response);
        }
        break;
    }

    case BMS_RS485_REQ_CELLS: {
        struct bms_telemetry_view view;

        ret = bms_communications_collect_view(c, &view);
        if (ret != 0) {
            break;
        }

        response.type = BMS_RS485_RSP_CELLS;

        size_t len = 0U;
        ret = bms_protocol_encode_cells_payload(
            &view,
            response.payload,
            sizeof(response.payload),
            &len);

        if (ret == 0) {
            response.payload_len = (uint16_t)len;
            ret = send_rs485_frame(c, &response);
        }
        break;
    }

    case BMS_RS485_REQ_CONFIG: {
        if (c->config_ops == NULL ||
            c->config_ops->get_applied == NULL) {
            ret = -ENOTSUP;
            break;
        }

        struct bms_app_config cfg;

        ret = c->config_ops->get_applied(c->config_context,
                                         &cfg);
        if (ret != 0) {
            break;
        }

        response.type = BMS_RS485_RSP_CONFIG;

        size_t len = 0U;
        ret = bms_protocol_encode_config_payload(
            &cfg,
            response.payload,
            sizeof(response.payload),
            &len);

        if (ret == 0) {
            response.payload_len = (uint16_t)len;
            ret = send_rs485_frame(c, &response);
        }
        break;
    }

    case BMS_RS485_REQ_SET_STAGED_CONFIG: {
        if (!c->allow_remote_config_write) {
            ret = -EACCES;
            break;
        }

        if (c->config_ops == NULL ||
            c->config_ops->stage == NULL) {
            ret = -ENOTSUP;
            break;
        }

        struct bms_app_config cfg;

        ret = bms_protocol_decode_config_payload(
            request.payload,
            request.payload_len,
            &cfg);

        if (ret == 0) {
            ret = c->config_ops->stage(c->config_context, &cfg);
        }

        if (ret == 0) {
            ret = send_result(c, request.sequence, 0);
        }
        break;
    }

    case BMS_RS485_REQ_APPLY_CONFIG:
        if (!c->allow_remote_config_write) {
            ret = -EACCES;
            break;
        }

        if (c->config_ops == NULL ||
            c->config_ops->apply == NULL) {
            ret = -ENOTSUP;
            break;
        }

        ret = c->config_ops->apply(c->config_context);

        if (ret == 0) {
            ret = send_result(c, request.sequence, 0);
        }
        break;

    case BMS_RS485_REQ_SAVE_CONFIG:
        if (!c->allow_remote_config_write) {
            ret = -EACCES;
            break;
        }

        if (c->config_ops == NULL ||
            c->config_ops->save == NULL) {
            ret = -ENOTSUP;
            break;
        }

        ret = c->config_ops->save(c->config_context);

        if (ret == 0) {
            ret = send_result(c, request.sequence, 0);
        }
        break;

    default:
        ret = -ENOTSUP;
        break;
    }

    /*
     * For command failures, send a protocol-level error response when
     * possible. Preserve the original errno as the function return value.
     */
    if (ret != 0 &&
        request.type != BMS_RS485_REQ_STATUS &&
        request.type != BMS_RS485_REQ_CELLS &&
        request.type != BMS_RS485_REQ_CONFIG) {
        int original = ret;
        (void)send_result(c, request.sequence, original);
        record_error(c, original);
        return original;
    }

    if (ret != 0) {
        record_error(c, ret);
    }

    return ret;
}

static void communication_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);
    struct bms_communications *c =
        CONTAINER_OF(dwork, struct bms_communications, work);

    if (atomic_get(&c->running) == 0) {
        return;
    }

    (void)bms_communications_publish_can_now(c);

    if (atomic_get(&c->running) != 0) {
        (void)k_work_schedule(&c->work,
                              K_MSEC(c->can_period_ms));
    }
}

int bms_communications_init(
    struct bms_communications *c,
    struct bms_measurement *measurement,
    struct bms_core_runtime *runtime,
    const struct bms_can_backend_ops *can_ops,
    void *can_context,
    const struct bms_rs485_backend_ops *rs485_ops,
    void *rs485_context,
    const struct bms_comm_config_ops *config_ops,
    void *config_context,
    bool allow_remote_config_write,
    uint32_t can_period_ms)
{
    if (c == NULL || measurement == NULL || runtime == NULL ||
        can_period_ms == 0U) {
        return -EINVAL;
    }

    memset(c, 0, sizeof(*c));

    c->measurement = measurement;
    c->runtime = runtime;

    c->can_ops = can_ops;
    c->can_context = can_context;

    c->rs485_ops = rs485_ops;
    c->rs485_context = rs485_context;

    c->config_ops = config_ops;
    c->config_context = config_context;

    c->allow_remote_config_write = allow_remote_config_write;
    c->can_period_ms = can_period_ms;
    c->rs485_tx_sequence = 1U;

    k_mutex_init(&c->lock);
    k_work_init_delayable(&c->work,
                          communication_work_handler);
    atomic_clear(&c->running);

    return 0;
}

int bms_communications_start(struct bms_communications *c)
{
    if (c == NULL) {
        return -EINVAL;
    }

    if (!atomic_cas(&c->running, 0, 1)) {
        return 0;
    }

    return k_work_schedule(&c->work, K_NO_WAIT) < 0 ?
           -EIO : 0;
}

int bms_communications_stop(struct bms_communications *c)
{
    if (c == NULL) {
        return -EINVAL;
    }

    atomic_clear(&c->running);
    (void)k_work_cancel_delayable(&c->work);
    return 0;
}

int bms_communications_get_stats(
    struct bms_communications *c,
    struct bms_communications_stats *out)
{
    if (c == NULL || out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&c->lock, K_FOREVER);
    *out = c->stats;
    k_mutex_unlock(&c->lock);

    return 0;
}
