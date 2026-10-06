#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bms/bms_config_store.h"

#define HEADER_SIZE 16U

static void put_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFFU);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFFU);
    *p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    for (unsigned i = 0U; i < 4U; ++i) {
        (*p)[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
    *p += 4;
}

static void put_u64(uint8_t **p, uint64_t v)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        (*p)[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
    *p += 8;
}

static void put_i32(uint8_t **p, int32_t v)
{
    put_u32(p, (uint32_t)v);
}

static void put_i64(uint8_t **p, int64_t v)
{
    put_u64(p, (uint64_t)v);
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

static uint64_t get_u64(const uint8_t **p)
{
    uint64_t v = 0U;
    for (unsigned i = 0U; i < 8U; ++i) {
        v |= (uint64_t)(*p)[i] << (8U * i);
    }
    *p += 8;
    return v;
}

static int32_t get_i32(const uint8_t **p)
{
    return (int32_t)get_u32(p);
}

static int64_t get_i64(const uint8_t **p)
{
    return (int64_t)get_u64(p);
}

/* Standard reflected CRC-32 (poly 0xEDB88320). */
static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];

        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static int encode_payload(const struct bms_persistent_data *in,
                          uint8_t *buffer,
                          size_t capacity,
                          size_t *out_len)
{
    if (in == NULL || buffer == NULL || out_len == NULL) {
        return -EINVAL;
    }

    int ret = bms_config_validate(&in->config);
    if (ret != 0) {
        return ret;
    }

    /*
     * Current schema fits comfortably below this. Keep an explicit capacity
     * check anyway so future fields cannot silently overflow.
     */
    if (capacity < 160U) {
        return -ENOSPC;
    }

    uint8_t *p = buffer;
    const struct bms_app_config *c = &in->config;

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

    *p++ = in->estimator_state_valid ? 1U : 0U;

    const struct bms_estimator_persisted_state *s =
        &in->estimator_state;

    put_i64(&p, s->remaining_charge_uAh);
    put_u32(&p, s->estimated_full_capacity_mAh);

    put_u64(&p, s->charged_uAh);
    put_u64(&p, s->discharged_uAh);
    put_u64(&p, s->throughput_uAh);

    put_u64(&p, s->energy_in_uWh);
    put_u64(&p, s->energy_out_uWh);

    put_u32(&p, s->skipped_intervals);

    *out_len = (size_t)(p - buffer);
    return 0;
}

static int decode_payload(const uint8_t *buffer,
                          size_t len,
                          struct bms_persistent_data *out)
{
    if (buffer == NULL || out == NULL || len < 100U) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    const uint8_t *p = buffer;
    const uint8_t *end = buffer + len;

#define NEED(n) do { if ((size_t)(end - p) < (size_t)(n)) return -EBADMSG; } while (0)

    NEED(2); out->config.schema_version = get_u16(&p);
    NEED(4); out->config.measurement_period_ms = get_u32(&p);
    NEED(4); out->config.runtime_period_ms = get_u32(&p);

    NEED(4); out->config.protection.cell_uv_trip_mv = get_i32(&p);
    NEED(4); out->config.protection.cell_uv_release_mv = get_i32(&p);
    NEED(4); out->config.protection.cell_ov_trip_mv = get_i32(&p);
    NEED(4); out->config.protection.cell_ov_release_mv = get_i32(&p);
    NEED(4); out->config.protection.charge_oc_trip_ma = get_i32(&p);
    NEED(4); out->config.protection.charge_oc_release_ma = get_i32(&p);
    NEED(4); out->config.protection.discharge_oc_trip_ma = get_i32(&p);
    NEED(4); out->config.protection.discharge_oc_release_ma = get_i32(&p);
    NEED(4); out->config.protection.max_snapshot_age_ms = get_u32(&p);

    NEED(1); out->config.balancing.enabled = (*p++ != 0U);
    NEED(4); out->config.balancing.start_cell_mv = get_i32(&p);
    NEED(4); out->config.balancing.start_delta_mv = get_i32(&p);
    NEED(4); out->config.balancing.stop_delta_mv = get_i32(&p);
    NEED(4); out->config.balancing.max_abs_current_ma = get_i32(&p);
    NEED(1); out->config.balancing.max_simultaneous_cells = *p++;

    NEED(4); out->config.estimator.design_capacity_mAh = get_u32(&p);
    NEED(4); out->config.estimator.initial_full_capacity_mAh = get_u32(&p);
    NEED(2); out->config.estimator.initial_soc_centi_pct = get_u16(&p);
    NEED(2); out->config.estimator.charge_efficiency_permille = get_u16(&p);
    NEED(4); out->config.estimator.max_integration_interval_ms = get_u32(&p);

    NEED(1); out->estimator_state_valid = (*p++ != 0U);

    NEED(8); out->estimator_state.remaining_charge_uAh = get_i64(&p);
    NEED(4); out->estimator_state.estimated_full_capacity_mAh = get_u32(&p);

    NEED(8); out->estimator_state.charged_uAh = get_u64(&p);
    NEED(8); out->estimator_state.discharged_uAh = get_u64(&p);
    NEED(8); out->estimator_state.throughput_uAh = get_u64(&p);

    NEED(8); out->estimator_state.energy_in_uWh = get_u64(&p);
    NEED(8); out->estimator_state.energy_out_uWh = get_u64(&p);

    NEED(4); out->estimator_state.skipped_intervals = get_u32(&p);

#undef NEED

    if (p != end) {
        return -EBADMSG;
    }

    return bms_config_validate(&out->config);
}

int bms_config_store_init(
    struct bms_config_store *store,
    const struct bms_config_store_backend_ops *ops,
    void *backend_context)
{
    if (store == NULL || ops == NULL ||
        ops->read == NULL || ops->write == NULL || ops->erase == NULL) {
        return -EINVAL;
    }

    memset(store, 0, sizeof(*store));
    store->ops = ops;
    store->backend_context = backend_context;
    return 0;
}

int bms_config_store_save(struct bms_config_store *store,
                          const struct bms_persistent_data *data)
{
    if (store == NULL || store->ops == NULL || data == NULL) {
        return -EINVAL;
    }

    uint8_t image[BMS_CONFIG_STORE_MAX_IMAGE_SIZE];
    uint8_t payload[BMS_CONFIG_STORE_MAX_IMAGE_SIZE - HEADER_SIZE];
    size_t payload_len = 0U;

    int ret = encode_payload(data,
                             payload,
                             sizeof(payload),
                             &payload_len);
    if (ret != 0) {
        return ret;
    }

    if (HEADER_SIZE + payload_len > sizeof(image) ||
        payload_len > UINT16_MAX) {
        return -E2BIG;
    }

    uint32_t generation = store->last_generation + 1U;
    uint32_t crc = crc32_bytes(payload, payload_len);

    uint8_t *p = image;
    put_u32(&p, BMS_CONFIG_STORE_MAGIC);
    put_u16(&p, BMS_CONFIG_STORE_FORMAT_VERSION);
    put_u16(&p, (uint16_t)payload_len);
    put_u32(&p, generation);
    put_u32(&p, crc);

    memcpy(p, payload, payload_len);

    ret = store->ops->write(store->backend_context,
                            image,
                            HEADER_SIZE + payload_len);
    if (ret == 0) {
        store->last_generation = generation;
    }

    return ret;
}

int bms_config_store_load(struct bms_config_store *store,
                          struct bms_persistent_data *data)
{
    if (store == NULL || store->ops == NULL || data == NULL) {
        return -EINVAL;
    }

    uint8_t image[BMS_CONFIG_STORE_MAX_IMAGE_SIZE];
    size_t image_len = 0U;

    int ret = store->ops->read(store->backend_context,
                               image,
                               sizeof(image),
                               &image_len);
    if (ret != 0) {
        return ret;
    }

    if (image_len < HEADER_SIZE) {
        return -EBADMSG;
    }

    const uint8_t *p = image;

    uint32_t magic = get_u32(&p);
    uint16_t format_version = get_u16(&p);
    uint16_t payload_len = get_u16(&p);
    uint32_t generation = get_u32(&p);
    uint32_t stored_crc = get_u32(&p);

    if (magic != BMS_CONFIG_STORE_MAGIC) {
        return -EBADMSG;
    }

    if (format_version != BMS_CONFIG_STORE_FORMAT_VERSION) {
        return -EPROTONOSUPPORT;
    }

    if ((size_t)payload_len + HEADER_SIZE != image_len) {
        return -EBADMSG;
    }

    uint32_t calculated_crc = crc32_bytes(p, payload_len);
    if (calculated_crc != stored_crc) {
        return -EBADMSG;
    }

    ret = decode_payload(p, payload_len, data);
    if (ret == 0) {
        store->last_generation = generation;
    }

    return ret;
}

int bms_config_store_erase(struct bms_config_store *store)
{
    if (store == NULL || store->ops == NULL) {
        return -EINVAL;
    }

    int ret = store->ops->erase(store->backend_context);
    if (ret == 0) {
        store->last_generation = 0U;
    }

    return ret;
}

uint32_t bms_config_store_last_generation(
    const struct bms_config_store *store)
{
    return store == NULL ? 0U : store->last_generation;
}
