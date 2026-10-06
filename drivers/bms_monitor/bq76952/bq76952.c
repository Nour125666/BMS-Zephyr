#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>

#include "bms/bms_types.h"
#include "bq76952.h"
#include "bq76952_reg.h"

int bq76952_init(struct bq76952_device *d, uint8_t id, uint8_t cells,
                 uint16_t v_lsb_mv, uint32_t i_lsb_ua)
{
    if (!d || cells == 0 || cells > 16) return -EINVAL;
    memset(d, 0, sizeof(*d));
    d->cell_count = cells;
    d->user_volt_lsb_mv = v_lsb_mv;
    d->user_amp_lsb_ua = i_lsb_ua;
    return bq76952_transport_init(&d->transport, id);
}

static int cell(struct bq76952_device *d, uint8_t idx, int16_t *mv)
{
    return bq76952_direct_read_i16(&d->transport,
                                   (uint8_t)(BQ_CMD_CELL1 + 2U * idx), mv);
}

static int user_v(struct bq76952_device *d, uint8_t cmd, int32_t *mv)
{
    int16_t raw;
    int ret = bq76952_direct_read_i16(&d->transport, cmd, &raw);
    if (ret) return ret;
    *mv = (int32_t)raw * d->user_volt_lsb_mv;
    return 0;
}

int bq76952_read_all_measurements(struct bq76952_device *d,
                                  struct bq76952_measurements *m)
{
    if (!d || !m) return -EINVAL;
    memset(m, 0, sizeof(*m));
    m->cell_count = d->cell_count;

    for (uint8_t i = 0; i < d->cell_count; ++i) {
        int ret = cell(d, i, &m->cell_mv[i]);
        if (ret) return ret;
    }

    int ret = user_v(d, BQ_CMD_STACK_VOLTAGE, &m->stack_mv); if (ret) return ret;
    ret = user_v(d, BQ_CMD_PACK_VOLTAGE, &m->pack_mv); if (ret) return ret;
    ret = user_v(d, BQ_CMD_LD_VOLTAGE, &m->ld_mv); if (ret) return ret;

    int16_t raw_i;
    ret = bq76952_direct_read_i16(&d->transport, BQ_CMD_CC2_CURRENT, &raw_i);
    if (ret) return ret;
    m->current_ma = ((int32_t)raw_i * (int32_t)d->user_amp_lsb_ua) / 1000;
    return 0;
}

int bq76952_read_faults(struct bq76952_device *d, struct bq76952_faults *f)
{
    if (!d || !f) return -EINVAL;
    memset(f, 0, sizeof(*f));

    int ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_ALERT_A, &f->safety_alert_a); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_STATUS_A, &f->safety_status_a); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_ALERT_B, &f->safety_alert_b); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_STATUS_B, &f->safety_status_b); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_ALERT_C, &f->safety_alert_c); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_SAFETY_STATUS_C, &f->safety_status_c); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_PF_STATUS_A, &f->pf_status_a); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_PF_STATUS_B, &f->pf_status_b); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_PF_STATUS_C, &f->pf_status_c); if (ret) return ret;
    ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_PF_STATUS_D, &f->pf_status_d); if (ret) return ret;

    if (f->safety_status_a & (1U << 2)) f->flags |= BMS_FAULT_CELL_UNDERVOLTAGE;
    if (f->safety_status_a & (1U << 3)) f->flags |= BMS_FAULT_CELL_OVERVOLTAGE;
    if (f->safety_status_a & (1U << 4)) f->flags |= BMS_FAULT_OVERCURRENT_CHARGE;
    if (f->safety_status_a & ((1U << 5) | (1U << 6))) f->flags |= BMS_FAULT_OVERCURRENT_DISCHARGE;
    if (f->safety_status_a & (1U << 7)) f->flags |= BMS_FAULT_SHORT_CIRCUIT_DISCHARGE;
    if (f->pf_status_a || f->pf_status_b || f->pf_status_c || f->pf_status_d)
        f->flags |= BMS_FAULT_PERMANENT_FAIL;

    return 0;
}

int bq76952_read_fet_state(struct bq76952_device *d, struct bq76952_fet_state *f)
{
    uint8_t raw;
    int ret = bq76952_direct_read_u8(&d->transport, BQ_CMD_FET_STATUS, &raw);
    if (ret) return ret;
    memset(f, 0, sizeof(*f));
    f->raw = raw;
    f->chg  = (raw & (1U << 0)) != 0;
    f->pchg = (raw & (1U << 1)) != 0;
    f->dsg  = (raw & (1U << 2)) != 0;
    f->pdsg = (raw & (1U << 3)) != 0;
    return 0;
}

int bq76952_get_balance_mask(struct bq76952_device *d, uint16_t *mask)
{
    uint8_t raw[2]; size_t n = 0;
    int ret = bq76952_subcmd_read(&d->transport, BQ_SUBCMD_CB_ACTIVE,
                                  raw, sizeof(raw), &n);
    if (ret) return ret;
    if (n != 2U) return -EPROTO;
    *mask = sys_get_le16(raw);
    return 0;
}

int bq76952_set_balance_mask(struct bq76952_device *d, uint16_t mask)
{
    uint8_t raw[2];
    sys_put_le16(mask, raw);
    return bq76952_subcmd_write(&d->transport, BQ_SUBCMD_CB_ACTIVE, raw, 2U);
}

int bq76952_force_all_off(struct bq76952_device *d)
{ return bq76952_subcmd_command(&d->transport, BQ_SUBCMD_ALL_OFF); }
int bq76952_force_charge_off(struct bq76952_device *d)
{ return bq76952_subcmd_command(&d->transport, BQ_SUBCMD_CHG_OFF); }
int bq76952_force_discharge_off(struct bq76952_device *d)
{ return bq76952_subcmd_command(&d->transport, BQ_SUBCMD_DSG_OFF); }
int bq76952_release_fets(struct bq76952_device *d)
{ return bq76952_subcmd_command(&d->transport, BQ_SUBCMD_ALL_ON); }
