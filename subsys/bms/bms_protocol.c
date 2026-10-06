#include <errno.h>
#include <limits.h>
#include <string.h>

#include "bms/bms_protocol.h"

static void put_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFFU);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFFU);
    *p += 2;
}

static void put_i16(uint8_t **p, int16_t v)
{
    put_u16(p, (uint16_t)v);
}

static void put_u32(uint8_t **p, uint32_t v)
{
    for (unsigned i = 0U; i < 4U; ++i) {
        (*p)[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
    *p += 4;
}

static void put_i32(uint8_t **p, int32_t v)
{
    put_u32(p, (uint32_t)v);
}

static void put_u64(uint8_t **p, uint64_t v)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        (*p)[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
    *p += 8;
}

static uint16_t get_u16(const uint8_t **p)
{
    uint16_t v = (uint16_t)(*p)[0] |
                 ((uint16_t)(*p)[1] << 8);
    *p += 2;
    return v;
}


static uint32_t get_u32(const uint8_t **p)
{
    uint32_t v = 0U;

    for (unsigned i = 0U; i < 4U; ++i) {
        v |= (uint32_t)(*p)[i] << (8U * i);
    }

    *p += 4;
    return v;
}

static int32_t get_i32(const uint8_t **p)
{
    return (int32_t)get_u32(p);
}

static uint64_t get_u64(const uint8_t **p)
{
    uint64_t v = 0U;

    for (unsigned i = 0U; i < 8U; ++i) {
        v |= (uint64_t)(*p)[i] << (8U * i);
    }

    *p += 8;
    return v;
}

/* CRC16-CCITT-FALSE: poly=0x1021, init=0xFFFF. */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0U; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;

        for (unsigned bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

static uint16_t clamp_u16_div(int32_t value, int32_t divisor)
{
    if (value <= 0 || divisor <= 0) {
        return 0U;
    }

    uint32_t scaled = (uint32_t)value / (uint32_t)divisor;

    if (scaled > UINT16_MAX) {
        scaled = UINT16_MAX;
    }

    return (uint16_t)scaled;
}

static int16_t clamp_i16_div(int32_t value, int32_t divisor)
{
    if (divisor <= 0) {
        return 0;
    }

    int32_t scaled = value / divisor;

    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }

    return (int16_t)scaled;
}

int bms_protocol_build_can_frames(
    const struct bms_telemetry_view *v,
    struct bms_can_frame *frames,
    size_t max_frames,
    size_t *out_count)
{
    if (v == NULL || frames == NULL || out_count == NULL) {
        return -EINVAL;
    }

    size_t cell_frames = ((size_t)v->cell_count + 3U) / 4U;
    size_t required = 5U + cell_frames;

    if (v->cell_count > CONFIG_BMS_MAX_CELLS ||
        required > max_frames ||
        required > BMS_CAN_MAX_FRAMES) {
        return -ENOSPC;
    }

    memset(frames, 0, sizeof(*frames) * required);

    /*
     * 0x500 PACK STATUS
     * bytes 0..1: pack voltage / 10 mV
     * bytes 2..3: current / 100 mA, signed
     * bytes 4..5: SOC / 0.1%
     * byte  6   : operating state
     * byte  7   : flags:
     *             bit0 charge allowed
     *             bit1 discharge allowed
     *             bit2 CHG switch state
     *             bit3 DSG switch state
     */
    frames[0].id = BMS_CAN_ID_PACK_STATUS;
    frames[0].dlc = 8U;
    uint8_t *p = frames[0].data;
    put_u16(&p, clamp_u16_div(v->pack_mv, 10));
    put_i16(&p, clamp_i16_div(v->current_ma, 100));
    put_u16(&p, (uint16_t)(v->soc_centi_pct / 10U));
    *p++ = (uint8_t)v->state;

    uint8_t flags = 0U;
    if (v->charge_allowed) flags |= (1U << 0);
    if (v->discharge_allowed) flags |= (1U << 1);
    if (v->charge_switch_on) flags |= (1U << 2);
    if (v->discharge_switch_on) flags |= (1U << 3);
    *p = flags;

    /*
     * 0x501 CELL MIN/MAX
     * mV values are sent directly.
     */
    frames[1].id = BMS_CAN_ID_CELL_MINMAX;
    frames[1].dlc = 8U;
    p = frames[1].data;
    put_u16(&p, clamp_u16_div(v->min_cell_mv, 1));
    put_u16(&p, clamp_u16_div(v->max_cell_mv, 1));
    *p++ = (uint8_t)(v->min_cell_index + 1U);
    *p++ = (uint8_t)(v->max_cell_index + 1U);
    *p++ = v->cell_count;
    *p = 0U;

    frames[2].id = BMS_CAN_ID_FAULTS_LOW;
    frames[2].dlc = 8U;
    p = frames[2].data;
    put_u32(&p, (uint32_t)(v->faults & 0xFFFFFFFFULL));
    put_u32(&p, (uint32_t)(v->balance_mask & 0xFFFFFFFFULL));

    frames[3].id = BMS_CAN_ID_FAULTS_HIGH;
    frames[3].dlc = 8U;
    p = frames[3].data;
    put_u32(&p, (uint32_t)(v->faults >> 32));
    put_u32(&p, (uint32_t)(v->balance_mask >> 32));

    /*
     * 0x504 ESTIMATOR
     * SOH in 0.1%, estimated usable capacity in 100 mAh units,
     * EFC in milli-cycles.
     */
    frames[4].id = BMS_CAN_ID_ESTIMATOR;
    frames[4].dlc = 8U;
    p = frames[4].data;
    put_u16(&p, (uint16_t)(v->soh_centi_pct / 10U));

    uint32_t full_100mAh = v->estimated_full_capacity_mAh / 100U;
    if (full_100mAh > UINT16_MAX) {
        full_100mAh = UINT16_MAX;
    }
    put_u16(&p, (uint16_t)full_100mAh);
    put_u32(&p, v->equivalent_full_cycles_milli);

    /*
     * Cell frames: four direct-mV cell values per 8-byte CAN frame.
     * 0x510 = cells 1..4, 0x511 = 5..8, ...
     */
    for (size_t chunk = 0U; chunk < cell_frames; ++chunk) {
        struct bms_can_frame *f = &frames[5U + chunk];
        f->id = BMS_CAN_CELL_FRAME_BASE + (uint32_t)chunk;
        f->dlc = 8U;

        p = f->data;

        for (size_t slot = 0U; slot < 4U; ++slot) {
            size_t cell = chunk * 4U + slot;
            uint16_t mv = 0U;

            if (cell < v->cell_count) {
                mv = clamp_u16_div(v->cell_mv[cell], 1);
            }

            put_u16(&p, mv);
        }
    }

    *out_count = required;
    return 0;
}

int bms_protocol_rs485_encode(
    const struct bms_rs485_frame *frame,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len)
{
    if (frame == NULL || out == NULL || out_len == NULL ||
        frame->payload_len > BMS_RS485_MAX_PAYLOAD) {
        return -EINVAL;
    }

    size_t total = 2U + 1U + 1U + 2U + 2U +
                   frame->payload_len + 2U;

    if (out_capacity < total) {
        return -ENOSPC;
    }

    uint8_t *p = out;

    *p++ = BMS_RS485_SOF0;
    *p++ = BMS_RS485_SOF1;
    *p++ = BMS_PROTOCOL_VERSION;
    *p++ = frame->type;
    put_u16(&p, frame->sequence);
    put_u16(&p, frame->payload_len);

    memcpy(p, frame->payload, frame->payload_len);
    p += frame->payload_len;

    /*
     * CRC covers version..payload, excluding SOF and CRC itself.
     */
    uint16_t crc = crc16_ccitt(&out[2], total - 4U);
    put_u16(&p, crc);

    *out_len = total;
    return 0;
}

int bms_protocol_rs485_decode(
    const uint8_t *data,
    size_t len,
    struct bms_rs485_frame *out)
{
    if (data == NULL || out == NULL || len < 10U) {
        return -EINVAL;
    }

    if (data[0] != BMS_RS485_SOF0 ||
        data[1] != BMS_RS485_SOF1) {
        return -EBADMSG;
    }

    if (data[2] != BMS_PROTOCOL_VERSION) {
        return -EPROTONOSUPPORT;
    }

    const uint8_t *p = &data[3];

    uint8_t type = *p++;
    uint16_t sequence = get_u16(&p);
    uint16_t payload_len = get_u16(&p);

    size_t expected = 2U + 1U + 1U + 2U + 2U +
                      payload_len + 2U;

    if (payload_len > BMS_RS485_MAX_PAYLOAD ||
        expected != len) {
        return -EBADMSG;
    }

    uint16_t stored_crc =
        (uint16_t)data[len - 2U] |
        ((uint16_t)data[len - 1U] << 8);

    uint16_t calculated_crc = crc16_ccitt(&data[2], len - 4U);

    if (stored_crc != calculated_crc) {
        return -EBADMSG;
    }

    memset(out, 0, sizeof(*out));
    out->type = type;
    out->sequence = sequence;
    out->payload_len = payload_len;

    memcpy(out->payload, p, payload_len);

    return 0;
}

int bms_protocol_encode_status_payload(
    const struct bms_telemetry_view *v,
    uint8_t *out,
    size_t capacity,
    size_t *out_len)
{
    if (v == NULL || out == NULL || out_len == NULL) {
        return -EINVAL;
    }

    const size_t needed = 4U + 4U + 4U + 4U + 4U +
                          1U + 1U + 1U +
                          8U + 8U +
                          2U + 2U + 4U + 4U +
                          1U + 1U;

    if (capacity < needed) {
        return -ENOSPC;
    }

    uint8_t *p = out;

    put_u32(&p, v->snapshot_sequence);
    put_i32(&p, v->pack_mv);
    put_i32(&p, v->current_ma);
    put_i32(&p, v->min_cell_mv);
    put_i32(&p, v->max_cell_mv);

    *p++ = (uint8_t)(v->min_cell_index + 1U);
    *p++ = (uint8_t)(v->max_cell_index + 1U);
    *p++ = v->cell_count;

    put_u64(&p, v->faults);
    put_u64(&p, v->balance_mask);

    put_u16(&p, v->soc_centi_pct);
    put_u16(&p, v->soh_centi_pct);
    put_u32(&p, v->estimated_full_capacity_mAh);
    put_u32(&p, v->equivalent_full_cycles_milli);

    *p++ = (uint8_t)v->state;

    uint8_t flags = 0U;
    if (v->charge_allowed) flags |= 1U << 0;
    if (v->discharge_allowed) flags |= 1U << 1;
    if (v->charge_switch_on) flags |= 1U << 2;
    if (v->discharge_switch_on) flags |= 1U << 3;
    *p++ = flags;

    *out_len = (size_t)(p - out);
    return 0;
}

int bms_protocol_decode_status_payload(
    const uint8_t *data,
    size_t len,
    struct bms_telemetry_view *out)
{
    if (data == NULL || out == NULL || len < 53U) {
        return -EINVAL;
    }

    const uint8_t *p = data;
    memset(out, 0, sizeof(*out));

    out->snapshot_sequence = get_u32(&p);
    out->pack_mv = get_i32(&p);
    out->current_ma = get_i32(&p);
    out->min_cell_mv = get_i32(&p);
    out->max_cell_mv = get_i32(&p);

    out->min_cell_index = (uint8_t)(*p++ - 1U);
    out->max_cell_index = (uint8_t)(*p++ - 1U);
    out->cell_count = *p++;

    out->faults = get_u64(&p);
    out->balance_mask = get_u64(&p);

    out->soc_centi_pct = get_u16(&p);
    out->soh_centi_pct = get_u16(&p);
    out->estimated_full_capacity_mAh = get_u32(&p);
    out->equivalent_full_cycles_milli = get_u32(&p);

    out->state = *p++;

    uint8_t flags = *p++;
    out->charge_allowed = (flags & (1U << 0)) != 0U;
    out->discharge_allowed = (flags & (1U << 1)) != 0U;
    out->charge_switch_on = (flags & (1U << 2)) != 0U;
    out->discharge_switch_on = (flags & (1U << 3)) != 0U;

    return 0;
}

int bms_protocol_encode_cells_payload(
    const struct bms_telemetry_view *v,
    uint8_t *out,
    size_t capacity,
    size_t *out_len)
{
    if (v == NULL || out == NULL || out_len == NULL ||
        v->cell_count > CONFIG_BMS_MAX_CELLS) {
        return -EINVAL;
    }

    size_t needed = 1U + (size_t)v->cell_count * 2U;

    if (capacity < needed) {
        return -ENOSPC;
    }

    uint8_t *p = out;
    *p++ = v->cell_count;

    for (uint8_t i = 0U; i < v->cell_count; ++i) {
        put_u16(&p, clamp_u16_div(v->cell_mv[i], 1));
    }

    *out_len = needed;
    return 0;
}

int bms_protocol_decode_cells_payload(
    const uint8_t *data,
    size_t len,
    struct bms_telemetry_view *out)
{
    if (data == NULL || out == NULL || len < 1U) {
        return -EINVAL;
    }

    uint8_t count = data[0];

    if (count > CONFIG_BMS_MAX_CELLS ||
        len != 1U + (size_t)count * 2U) {
        return -EBADMSG;
    }

    const uint8_t *p = &data[1];

    memset(out, 0, sizeof(*out));
    out->cell_count = count;

    for (uint8_t i = 0U; i < count; ++i) {
        out->cell_mv[i] = get_u16(&p);
    }

    return 0;
}

int bms_protocol_encode_config_payload(
    const struct bms_app_config *c,
    uint8_t *out,
    size_t capacity,
    size_t *out_len)
{
    if (c == NULL || out == NULL || out_len == NULL) {
        return -EINVAL;
    }

    int ret = bms_config_validate(c);
    if (ret != 0) {
        return ret;
    }

    if (capacity < 96U) {
        return -ENOSPC;
    }

    uint8_t *p = out;

    put_u16(&p, c->schema_version);

    put_u32(&p, c->measurement_period_ms);
    put_u32(&p, c->runtime_period_ms);

    put_i32(&p, c->protection.cell_uv_trip_mv);
    put_i32(&p, c->protection.cell_uv_release_mv);
    put_i32(&p, c->protection.cell_ov_trip_mv);
    put_i32(&p, c->protection.cell_ov_release_mv);
    put_i32(&p, c->protection.charge_oc_trip_ma);
    put_i32(&p, c->protection.charge_oc_release_ma);
    put_i32(&p, c->protection.discharge_oc_trip_ma);
    put_i32(&p, c->protection.discharge_oc_release_ma);
    put_u32(&p, c->protection.max_snapshot_age_ms);

    *p++ = c->balancing.enabled ? 1U : 0U;
    put_i32(&p, c->balancing.start_cell_mv);
    put_i32(&p, c->balancing.start_delta_mv);
    put_i32(&p, c->balancing.stop_delta_mv);
    put_i32(&p, c->balancing.max_abs_current_ma);
    *p++ = c->balancing.max_simultaneous_cells;

    put_u32(&p, c->estimator.design_capacity_mAh);
    put_u32(&p, c->estimator.initial_full_capacity_mAh);
    put_u16(&p, c->estimator.initial_soc_centi_pct);
    put_u16(&p, c->estimator.charge_efficiency_permille);
    put_u32(&p, c->estimator.max_integration_interval_ms);

    *out_len = (size_t)(p - out);
    return 0;
}

int bms_protocol_decode_config_payload(
    const uint8_t *data,
    size_t len,
    struct bms_app_config *out)
{
    if (data == NULL || out == NULL || len < 80U) {
        return -EINVAL;
    }

    const uint8_t *p = data;
    const uint8_t *end = data + len;

#define NEED(n) do { if ((size_t)(end - p) < (size_t)(n)) return -EBADMSG; } while (0)

    memset(out, 0, sizeof(*out));

    NEED(2); out->schema_version = get_u16(&p);

    NEED(4); out->measurement_period_ms = get_u32(&p);
    NEED(4); out->runtime_period_ms = get_u32(&p);

    NEED(4); out->protection.cell_uv_trip_mv = get_i32(&p);
    NEED(4); out->protection.cell_uv_release_mv = get_i32(&p);
    NEED(4); out->protection.cell_ov_trip_mv = get_i32(&p);
    NEED(4); out->protection.cell_ov_release_mv = get_i32(&p);
    NEED(4); out->protection.charge_oc_trip_ma = get_i32(&p);
    NEED(4); out->protection.charge_oc_release_ma = get_i32(&p);
    NEED(4); out->protection.discharge_oc_trip_ma = get_i32(&p);
    NEED(4); out->protection.discharge_oc_release_ma = get_i32(&p);
    NEED(4); out->protection.max_snapshot_age_ms = get_u32(&p);

    NEED(1); out->balancing.enabled = (*p++ != 0U);
    NEED(4); out->balancing.start_cell_mv = get_i32(&p);
    NEED(4); out->balancing.start_delta_mv = get_i32(&p);
    NEED(4); out->balancing.stop_delta_mv = get_i32(&p);
    NEED(4); out->balancing.max_abs_current_ma = get_i32(&p);
    NEED(1); out->balancing.max_simultaneous_cells = *p++;

    NEED(4); out->estimator.design_capacity_mAh = get_u32(&p);
    NEED(4); out->estimator.initial_full_capacity_mAh = get_u32(&p);
    NEED(2); out->estimator.initial_soc_centi_pct = get_u16(&p);
    NEED(2); out->estimator.charge_efficiency_permille = get_u16(&p);
    NEED(4); out->estimator.max_integration_interval_ms = get_u32(&p);

#undef NEED

    if (p != end) {
        return -EBADMSG;
    }

    return bms_config_validate(out);
}

const char *bms_rs485_message_type_name(uint8_t type)
{
    switch (type) {
    case BMS_RS485_REQ_STATUS:            return "REQ_STATUS";
    case BMS_RS485_REQ_CELLS:             return "REQ_CELLS";
    case BMS_RS485_REQ_CONFIG:            return "REQ_CONFIG";
    case BMS_RS485_REQ_SET_STAGED_CONFIG: return "REQ_SET_STAGED_CONFIG";
    case BMS_RS485_REQ_APPLY_CONFIG:      return "REQ_APPLY_CONFIG";
    case BMS_RS485_REQ_SAVE_CONFIG:       return "REQ_SAVE_CONFIG";
    case BMS_RS485_RSP_STATUS:            return "RSP_STATUS";
    case BMS_RS485_RSP_CELLS:             return "RSP_CELLS";
    case BMS_RS485_RSP_CONFIG:            return "RSP_CONFIG";
    case BMS_RS485_RSP_ACK:               return "RSP_ACK";
    case BMS_RS485_RSP_ERROR:             return "RSP_ERROR";
    default:                              return "UNKNOWN";
    }
}
