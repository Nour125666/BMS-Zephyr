#include <errno.h>
#include <string.h>
#include "bms_monitor_bq76952.h"

static struct bms_monitor_bq76952 *self(struct bms_monitor *m)
{
    return (struct bms_monitor_bq76952 *)m->backend;
}

static int rd_sample(struct bms_monitor *m, struct bms_monitor_sample *out)
{
    struct bq76952_measurements raw;
    int ret = bq76952_read_all_measurements(&self(m)->bq, &raw);
    if (ret) return ret;

    memset(out, 0, sizeof(*out));
    out->cell_count = raw.cell_count;
    for (uint8_t i = 0; i < raw.cell_count; ++i) out->cell_mv[i] = raw.cell_mv[i];
    out->stack_voltage_valid = true;
    out->stack_mv = raw.stack_mv;
    out->pack_voltage_valid = true;
    out->pack_mv = raw.pack_mv;
    out->current_valid = true;
    out->current_ma = raw.current_ma;

    out->min_cell_mv = out->max_cell_mv = out->cell_mv[0];
    for (uint8_t i = 1; i < out->cell_count; ++i) {
        if (out->cell_mv[i] < out->min_cell_mv) {
            out->min_cell_mv = out->cell_mv[i];
            out->min_cell_index = i;
        }
        if (out->cell_mv[i] > out->max_cell_mv) {
            out->max_cell_mv = out->cell_mv[i];
            out->max_cell_index = i;
        }
    }
    return 0;
}

static int rd_faults(struct bms_monitor *m, struct bms_monitor_fault_state *out)
{
    struct bq76952_faults raw;
    int ret = bq76952_read_faults(&self(m)->bq, &raw);
    if (ret) return ret;
    memset(out, 0, sizeof(*out));
    out->alerts = raw.flags;
    out->faults = raw.flags;
    out->permanent_fail = (raw.flags & BMS_FAULT_PERMANENT_FAIL) != 0U;
    return 0;
}

static int rd_switch(struct bms_monitor *m, struct bms_monitor_switch_state *out)
{
    struct bq76952_fet_state raw;
    int ret = bq76952_read_fet_state(&self(m)->bq, &raw);
    if (ret) return ret;
    memset(out, 0, sizeof(*out));
    out->charge_enabled = raw.chg;
    out->discharge_enabled = raw.dsg;
    out->precharge_enabled = raw.pchg;
    out->predischarge_enabled = raw.pdsg;
    return 0;
}

static int set_switch(struct bms_monitor *m, enum bms_monitor_switch_request r)
{
    switch (r) {
    case BMS_SWITCH_RELEASE:       return bq76952_release_fets(&self(m)->bq);
    case BMS_SWITCH_CHARGE_OFF:    return bq76952_force_charge_off(&self(m)->bq);
    case BMS_SWITCH_DISCHARGE_OFF: return bq76952_force_discharge_off(&self(m)->bq);
    case BMS_SWITCH_ALL_OFF:       return bq76952_force_all_off(&self(m)->bq);
    default: return -EINVAL;
    }
}

static int get_bal(struct bms_monitor *m, uint64_t *mask)
{
    uint16_t local;
    int ret = bq76952_get_balance_mask(&self(m)->bq, &local);
    if (!ret) *mask = local;
    return ret;
}

static int set_bal(struct bms_monitor *m, uint64_t mask)
{
    if (mask & ~0xFFFFULL) return -ERANGE;
    return bq76952_set_balance_mask(&self(m)->bq, (uint16_t)mask);
}

static const struct bms_monitor_ops ops = {
    .read_sample = rd_sample,
    .read_faults = rd_faults,
    .read_switch_state = rd_switch,
    .set_switch = set_switch,
    .get_balance_mask = get_bal,
    .set_balance_mask = set_bal,
};

int bms_monitor_bq76952_init(struct bms_monitor_bq76952 *x,
                             const char *name, uint8_t id)
{
    if (!x) return -EINVAL;
    memset(x, 0, sizeof(*x));
    int ret = bq76952_init(&x->bq, id, 16U, 10U, 10000U);
    if (ret) return ret;

    x->monitor.name = name;
    x->monitor.ops = &ops;
    x->monitor.backend = x;
    x->monitor.cell_count = 16U;
    x->monitor.capabilities =
        BMS_MON_CAP_CELL_VOLTAGE |
        BMS_MON_CAP_STACK_VOLTAGE |
        BMS_MON_CAP_PACK_VOLTAGE |
        BMS_MON_CAP_CURRENT |
        BMS_MON_CAP_LOCAL_PROTECTION |
        BMS_MON_CAP_BALANCING |
        BMS_MON_CAP_SWITCH_CONTROL;
    return 0;
}
