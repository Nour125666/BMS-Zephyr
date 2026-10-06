#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include "bq76952_transport.h"
#include "bq76952_reg.h"
#include "virtual_bq_model.h"

int bq76952_transport_init(struct bq76952_transport *t, uint8_t id)
{
    if (!t || !virtual_bq_get(id)) return -EINVAL;
    memset(t, 0, sizeof(*t));
    t->instance_id = id;
    k_mutex_init(&t->lock);
    return 0;
}

static int32_t sum_cells(const struct virtual_bq_model *m)
{
    int32_t s = 0;
    for (uint8_t i = 0; i < 16; ++i) s += m->cell_mv[i];
    return s;
}

int bq76952_direct_read(struct bq76952_transport *t,
                        uint8_t cmd, void *data, size_t len)
{
    if (!t || !data) return -EINVAL;
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;
    uint8_t *out = data;

    if (cmd >= BQ_CMD_CELL1 && cmd <= 0x32 &&
        ((cmd - BQ_CMD_CELL1) % 2U) == 0U && len == 2U) {
        uint8_t idx = (cmd - BQ_CMD_CELL1) / 2U;
        sys_put_le16((uint16_t)m->cell_mv[idx], out);
        return 0;
    }

    if (len == 1U) {
        switch (cmd) {
        case BQ_CMD_SAFETY_ALERT_A: out[0] = m->safety_alert_a; return 0;
        case BQ_CMD_SAFETY_STATUS_A: out[0] = m->safety_status_a; return 0;
        case BQ_CMD_SAFETY_ALERT_B: out[0] = m->safety_alert_b; return 0;
        case BQ_CMD_SAFETY_STATUS_B: out[0] = m->safety_status_b; return 0;
        case BQ_CMD_SAFETY_ALERT_C: out[0] = m->safety_alert_c; return 0;
        case BQ_CMD_SAFETY_STATUS_C: out[0] = m->safety_status_c; return 0;
        case BQ_CMD_PF_STATUS_A: out[0] = m->pf_status_a; return 0;
        case BQ_CMD_PF_STATUS_B: out[0] = m->pf_status_b; return 0;
        case BQ_CMD_PF_STATUS_C: out[0] = m->pf_status_c; return 0;
        case BQ_CMD_PF_STATUS_D: out[0] = m->pf_status_d; return 0;
        case BQ_CMD_FET_STATUS: out[0] = m->fet_status; return 0;
        default: return -ENOTSUP;
        }
    }

    if (len == 2U) {
        int16_t raw;
        switch (cmd) {
        case BQ_CMD_STACK_VOLTAGE:
        case BQ_CMD_PACK_VOLTAGE:
        case BQ_CMD_LD_VOLTAGE:
            raw = (int16_t)(sum_cells(m) / 10);
            sys_put_le16((uint16_t)raw, out);
            return 0;
        case BQ_CMD_CC2_CURRENT:
            raw = (int16_t)(m->current_ma / 10);
            sys_put_le16((uint16_t)raw, out);
            return 0;
        default: return -ENOTSUP;
        }
    }
    return -EMSGSIZE;
}

int bq76952_direct_read_u8(struct bq76952_transport *t, uint8_t cmd, uint8_t *v)
{
    return bq76952_direct_read(t, cmd, v, 1U);
}

int bq76952_direct_read_i16(struct bq76952_transport *t, uint8_t cmd, int16_t *v)
{
    uint8_t raw[2];
    int ret = bq76952_direct_read(t, cmd, raw, 2U);
    if (!ret) *v = (int16_t)sys_get_le16(raw);
    return ret;
}

int bq76952_direct_write(struct bq76952_transport *t,
                           uint8_t cmd,
                           const void *data,
                           size_t len)
{
    ARG_UNUSED(t);
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(len);
    return -ENOTSUP;
}

int bq76952_direct_read_u16(struct bq76952_transport *t,
                            uint8_t cmd,
                            uint16_t *v)
{
    if (v == NULL) return -EINVAL;
    uint8_t raw[2];
    int ret = bq76952_direct_read(t, cmd, raw, sizeof(raw));
    if (!ret) *v = sys_get_le16(raw);
    return ret;
}

int bq76952_direct_write_u16(struct bq76952_transport *t,
                             uint8_t cmd,
                             uint16_t v)
{
    uint8_t raw[2];
    sys_put_le16(v, raw);
    return bq76952_direct_write(t, cmd, raw, sizeof(raw));
}

int bq76952_subcmd_command(struct bq76952_transport *t, uint16_t sub)
{
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;
    switch (sub) {
    case BQ_SUBCMD_ALL_OFF:
        m->fet_status &= (uint8_t)~((1U << 0) | (1U << 1) | (1U << 2) | (1U << 3));
        return 0;
    case BQ_SUBCMD_ALL_ON:
        m->fet_status |= (1U << 0) | (1U << 2);
        return 0;
    case BQ_SUBCMD_CHG_OFF:
        m->fet_status &= (uint8_t)~((1U << 0) | (1U << 1));
        return 0;
    case BQ_SUBCMD_DSG_OFF:
        m->fet_status &= (uint8_t)~((1U << 2) | (1U << 3));
        return 0;
    default: return -ENOTSUP;
    }
}

int bq76952_subcmd_read(struct bq76952_transport *t, uint16_t sub,
                        void *out, size_t cap, size_t *out_len)
{
    if (!out || !out_len) return -EINVAL;
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;

    if (sub == BQ_SUBCMD_DEVICE_NUMBER) {
        if (cap < 2U) return -EMSGSIZE;
        sys_put_le16(0x7695U, out);
        *out_len = 2U;
        return 0;
    }

    if (sub == BQ_SUBCMD_CB_ACTIVE) {
        if (cap < 2U) return -EMSGSIZE;
        sys_put_le16(m->balance_mask, out);
        *out_len = 2U;
        return 0;
    }

    return -ENOTSUP;
}

int bq76952_subcmd_write(struct bq76952_transport *t, uint16_t sub,
                         const void *payload, size_t len)
{
    if (!payload) return -EINVAL;
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;

    if (sub == BQ_SUBCMD_CB_ACTIVE && len == 2U) {
        m->balance_mask = sys_get_le16(payload);
        return 0;
    }
    return -ENOTSUP;
}

int bq76952_dm_read(struct bq76952_transport *t, uint16_t addr,
                    void *out, size_t len)
{
    if (!out) return -EINVAL;
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;

    if (addr == BQ_DM_DA_CONFIGURATION && len == 1U) {
        *(uint8_t *)out = m->da_configuration;
        return 0;
    }
    return -ENOTSUP;
}

int bq76952_dm_write(struct bq76952_transport *t, uint16_t addr,
                     const void *data, size_t len)
{
    if (!data) return -EINVAL;
    struct virtual_bq_model *m = virtual_bq_get(t->instance_id);
    if (!m) return -ENODEV;

    if (addr == BQ_DM_DA_CONFIGURATION && len == 1U) {
        m->da_configuration = *(const uint8_t *)data;
        return 0;
    }
    return -ENOTSUP;
}
