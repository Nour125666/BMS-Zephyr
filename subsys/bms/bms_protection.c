#include <errno.h>
#include <string.h>

#include "bms/bms_protection.h"

static bool config_valid(const struct bms_protection_config *c)
{
    if (c == NULL) {
        return false;
    }

    return c->cell_uv_trip_mv > 0 &&
           c->cell_uv_release_mv > c->cell_uv_trip_mv &&
           c->cell_ov_trip_mv > c->cell_ov_release_mv &&
           c->charge_oc_trip_ma > c->charge_oc_release_ma &&
           c->charge_oc_release_ma >= 0 &&
           c->discharge_oc_trip_ma > c->discharge_oc_release_ma &&
           c->discharge_oc_release_ma >= 0 &&
           c->max_snapshot_age_ms > 0U;
}

int bms_protection_init(struct bms_protection *p,
                        const struct bms_protection_config *config)
{
    if (p == NULL || !config_valid(config)) {
        return -EINVAL;
    }

    memset(p, 0, sizeof(*p));
    p->config = *config;
    p->latest.charge_allowed = false;
    p->latest.discharge_allowed = false;
    return 0;
}

static void update_hysteretic_bit(uint64_t *bits,
                                  uint64_t bit,
                                  bool trip_condition,
                                  bool release_condition)
{
    bool active = ((*bits & bit) != 0U);

    if (!active && trip_condition) {
        *bits |= bit;
    } else if (active && release_condition) {
        *bits &= ~bit;
    }
}

int bms_protection_evaluate(struct bms_protection *p,
                            const struct bms_snapshot *s,
                            int64_t snapshot_age_ms,
                            struct bms_protection_decision *out)
{
    if (p == NULL || s == NULL || out == NULL || s->cell_count == 0U) {
        return -EINVAL;
    }

    uint64_t sw = p->active_software_reasons;
    const struct bms_protection_config *c = &p->config;

    update_hysteretic_bit(&sw,
                          BMS_PROT_SW_CELL_UV,
                          s->min_cell_mv <= c->cell_uv_trip_mv,
                          s->min_cell_mv >= c->cell_uv_release_mv);

    update_hysteretic_bit(&sw,
                          BMS_PROT_SW_CELL_OV,
                          s->max_cell_mv >= c->cell_ov_trip_mv,
                          s->max_cell_mv <= c->cell_ov_release_mv);

    if (s->current_valid) {
        update_hysteretic_bit(&sw,
                              BMS_PROT_SW_CHARGE_OC,
                              s->current_ma >= c->charge_oc_trip_ma,
                              s->current_ma <= c->charge_oc_release_ma);

        update_hysteretic_bit(&sw,
                              BMS_PROT_SW_DISCHARGE_OC,
                              s->current_ma <= -c->discharge_oc_trip_ma,
                              s->current_ma >= -c->discharge_oc_release_ma);
    }

    if (snapshot_age_ms < 0 ||
        snapshot_age_ms > (int64_t)c->max_snapshot_age_ms) {
        sw |= BMS_PROT_STALE_SNAPSHOT;
    } else {
        sw &= ~BMS_PROT_STALE_SNAPSHOT;
    }

    p->active_software_reasons = sw;

    uint64_t reasons = sw;

    if ((s->faults & BMS_FAULT_CELL_UNDERVOLTAGE) != 0U) {
        reasons |= BMS_PROT_HW_CELL_UV;
    }
    if ((s->faults & BMS_FAULT_CELL_OVERVOLTAGE) != 0U) {
        reasons |= BMS_PROT_HW_CELL_OV;
    }
    if ((s->faults & BMS_FAULT_OVERCURRENT_CHARGE) != 0U) {
        reasons |= BMS_PROT_HW_CHARGE_OC;
    }
    if ((s->faults & BMS_FAULT_OVERCURRENT_DISCHARGE) != 0U) {
        reasons |= BMS_PROT_HW_DISCHARGE_OC;
    }
    if ((s->faults & BMS_FAULT_SHORT_CIRCUIT_DISCHARGE) != 0U) {
        reasons |= BMS_PROT_HW_SHORT_CIRCUIT;
    }
    if ((s->faults & BMS_FAULT_PERMANENT_FAIL) != 0U ||
        s->permanent_fail) {
        reasons |= BMS_PROT_HW_PERMANENT_FAIL;
    }

    const uint64_t block_charge =
        BMS_PROT_SW_CELL_OV |
        BMS_PROT_SW_CHARGE_OC |
        BMS_PROT_STALE_SNAPSHOT |
        BMS_PROT_HW_CELL_OV |
        BMS_PROT_HW_CHARGE_OC |
        BMS_PROT_HW_PERMANENT_FAIL;

    const uint64_t block_discharge =
        BMS_PROT_SW_CELL_UV |
        BMS_PROT_SW_DISCHARGE_OC |
        BMS_PROT_STALE_SNAPSHOT |
        BMS_PROT_HW_CELL_UV |
        BMS_PROT_HW_DISCHARGE_OC |
        BMS_PROT_HW_SHORT_CIRCUIT |
        BMS_PROT_HW_PERMANENT_FAIL;

    struct bms_protection_decision d = {
        .snapshot_sequence = s->sequence,
        .reasons = reasons,
        .charge_allowed = (reasons & block_charge) == 0U,
        .discharge_allowed = (reasons & block_discharge) == 0U,
    };

    p->latest = d;
    *out = d;

    return 0;
}

const char *bms_protection_reason_name(uint64_t bit)
{
    switch (bit) {
    case BMS_PROT_SW_CELL_UV:          return "SW_CELL_UV";
    case BMS_PROT_SW_CELL_OV:          return "SW_CELL_OV";
    case BMS_PROT_SW_CHARGE_OC:        return "SW_CHARGE_OC";
    case BMS_PROT_SW_DISCHARGE_OC:     return "SW_DISCHARGE_OC";
    case BMS_PROT_STALE_SNAPSHOT:      return "STALE_SNAPSHOT";
    case BMS_PROT_HW_CELL_UV:          return "HW_CELL_UV";
    case BMS_PROT_HW_CELL_OV:          return "HW_CELL_OV";
    case BMS_PROT_HW_CHARGE_OC:        return "HW_CHARGE_OC";
    case BMS_PROT_HW_DISCHARGE_OC:     return "HW_DISCHARGE_OC";
    case BMS_PROT_HW_SHORT_CIRCUIT:    return "HW_SHORT_CIRCUIT";
    case BMS_PROT_HW_PERMANENT_FAIL:   return "HW_PERMANENT_FAIL";
    default:                            return "UNKNOWN";
    }
}
