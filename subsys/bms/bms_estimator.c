#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "bms/bms_estimator.h"

#define CHARGE_SCALED_DENOMINATOR 3600000LL
#define CHARGE_RAW_DENOMINATOR       3600ULL
#define ENERGY_DENOMINATOR        3600000ULL

static bool config_valid(const struct bms_estimator_config *c)
{
    return c != NULL &&
           c->design_capacity_mAh > 0U &&
           c->initial_full_capacity_mAh > 0U &&
           c->initial_soc_centi_pct <= 10000U &&
           c->charge_efficiency_permille > 0U &&
           c->charge_efficiency_permille <= 1000U &&
           c->max_integration_interval_ms > 0U;
}

static int64_t full_capacity_uAh(const struct bms_estimator *e)
{
    return (int64_t)e->status.estimated_full_capacity_mAh * 1000LL;
}

static uint16_t clamp_u16(uint64_t value, uint16_t max_value)
{
    if (value > max_value) {
        return max_value;
    }
    return (uint16_t)value;
}

static void recompute_percentages(struct bms_estimator *e)
{
    int64_t full_uAh = full_capacity_uAh(e);

    if (full_uAh <= 0) {
        e->status.soc_centi_pct = 0U;
    } else {
        if (e->status.remaining_charge_uAh < 0) {
            e->status.remaining_charge_uAh = 0;
        }
        if (e->status.remaining_charge_uAh > full_uAh) {
            e->status.remaining_charge_uAh = full_uAh;
        }

        uint64_t soc =
            ((uint64_t)e->status.remaining_charge_uAh * 10000ULL) /
            (uint64_t)full_uAh;

        e->status.soc_centi_pct = clamp_u16(soc, 10000U);
    }

    uint64_t soh =
        ((uint64_t)e->status.estimated_full_capacity_mAh * 10000ULL) /
        e->status.design_capacity_mAh;

    /*
     * Allow a little headroom for a newly characterized cell exceeding its
     * label capacity, but avoid wrapping the public 16-bit status field.
     */
    e->status.soh_centi_pct = clamp_u16(soh, 20000U);

    if (full_uAh > 0) {
        uint64_t efc =
            (e->status.discharged_uAh * 1000ULL) / (uint64_t)full_uAh;

        if (efc > UINT32_MAX) {
            efc = UINT32_MAX;
        }
        e->status.equivalent_full_cycles_milli = (uint32_t)efc;
    } else {
        e->status.equivalent_full_cycles_milli = 0U;
    }
}

int bms_estimator_init(struct bms_estimator *e,
                       const struct bms_estimator_config *config)
{
    if (e == NULL || !config_valid(config)) {
        return -EINVAL;
    }

    memset(e, 0, sizeof(*e));
    e->config = *config;

    e->status.valid = true;
    e->status.design_capacity_mAh = config->design_capacity_mAh;
    e->status.estimated_full_capacity_mAh =
        config->initial_full_capacity_mAh;

    int64_t full_uAh = full_capacity_uAh(e);
    e->status.remaining_charge_uAh =
        (full_uAh * config->initial_soc_centi_pct) / 10000LL;

    recompute_percentages(e);
    return 0;
}

static uint64_t abs_i64_to_u64(int64_t x)
{
    if (x >= 0) {
        return (uint64_t)x;
    }

    /* Snapshot values are nowhere near INT64_MIN in this application. */
    return (uint64_t)(-x);
}

static uint64_t integrate_raw_uAh(uint64_t magnitude_mA,
                                  uint32_t dt_ms,
                                  uint64_t *remainder)
{
    uint64_t numerator =
        magnitude_mA * (uint64_t)dt_ms + *remainder;

    uint64_t delta = numerator / CHARGE_RAW_DENOMINATOR;
    *remainder = numerator % CHARGE_RAW_DENOMINATOR;

    return delta;
}

static uint64_t integrate_energy_uWh(uint64_t voltage_mV,
                                     uint64_t current_mA,
                                     uint32_t dt_ms,
                                     uint64_t *remainder)
{
    uint64_t numerator =
        voltage_mV * current_mA * (uint64_t)dt_ms + *remainder;

    uint64_t delta = numerator / ENERGY_DENOMINATOR;
    *remainder = numerator % ENERGY_DENOMINATOR;

    return delta;
}

int bms_estimator_process(struct bms_estimator *e,
                          const struct bms_snapshot *s,
                          struct bms_estimator_status *out)
{
    if (e == NULL || s == NULL) {
        return -EINVAL;
    }

    if (!e->status.valid) {
        return -EACCES;
    }

    /* The core runtime may run faster than measurement publication. */
    if (e->status.last_snapshot_sequence == s->sequence &&
        e->have_time_anchor) {
        if (out != NULL) {
            *out = e->status;
        }
        return 0;
    }

    if (!e->have_time_anchor) {
        e->have_time_anchor = true;
        e->last_timestamp_ms = s->captured_at_ms;
        e->status.last_snapshot_sequence = s->sequence;
        e->status.last_update_ms = s->captured_at_ms;

        if (out != NULL) {
            *out = e->status;
        }
        return 0;
    }

    int64_t dt64 = s->captured_at_ms - e->last_timestamp_ms;

    e->last_timestamp_ms = s->captured_at_ms;
    e->status.last_snapshot_sequence = s->sequence;
    e->status.last_update_ms = s->captured_at_ms;

    /*
     * Re-anchor after bad timing or missing current rather than integrating a
     * guessed interval.
     */
    if (dt64 <= 0 ||
        dt64 > (int64_t)e->config.max_integration_interval_ms ||
        !s->current_valid) {
        e->status.skipped_intervals++;
        if (out != NULL) {
            *out = e->status;
        }
        return 0;
    }

    uint32_t dt_ms = (uint32_t)dt64;
    int64_t current_ma = s->current_ma;

    /*
     * SOC charge integration:
     * positive current uses configurable coulombic efficiency;
     * discharge uses 1000 permille.
     */
    int64_t eff =
        current_ma >= 0 ?
        (int64_t)e->config.charge_efficiency_permille :
        1000LL;

    int64_t scaled =
        current_ma * (int64_t)dt_ms * eff +
        e->charge_remainder_scaled;

    int64_t delta_uAh = scaled / CHARGE_SCALED_DENOMINATOR;
    e->charge_remainder_scaled = scaled % CHARGE_SCALED_DENOMINATOR;

    e->status.remaining_charge_uAh += delta_uAh;

    uint64_t abs_current = abs_i64_to_u64(current_ma);

    if (current_ma > 0) {
        uint64_t delta =
            integrate_raw_uAh(abs_current,
                              dt_ms,
                              &e->charge_in_remainder);
        e->status.charged_uAh += delta;
    } else if (current_ma < 0) {
        uint64_t delta =
            integrate_raw_uAh(abs_current,
                              dt_ms,
                              &e->charge_out_remainder);
        e->status.discharged_uAh += delta;
    }

    if (current_ma != 0) {
        uint64_t delta =
            integrate_raw_uAh(abs_current,
                              dt_ms,
                              &e->throughput_remainder);
        e->status.throughput_uAh += delta;
    }

    if (s->pack_voltage_valid && s->pack_mv > 0 && current_ma != 0) {
        uint64_t voltage = (uint64_t)s->pack_mv;

        if (current_ma > 0) {
            e->status.energy_in_uWh +=
                integrate_energy_uWh(voltage,
                                     abs_current,
                                     dt_ms,
                                     &e->energy_in_remainder);
        } else {
            e->status.energy_out_uWh +=
                integrate_energy_uWh(voltage,
                                     abs_current,
                                     dt_ms,
                                     &e->energy_out_remainder);
        }
    }

    recompute_percentages(e);

    if (out != NULL) {
        *out = e->status;
    }

    return 0;
}

int bms_estimator_get_status(const struct bms_estimator *e,
                             struct bms_estimator_status *out)
{
    if (e == NULL || out == NULL) {
        return -EINVAL;
    }

    *out = e->status;
    return e->status.valid ? 0 : -ENODATA;
}

int bms_estimator_set_soc_centi_pct(struct bms_estimator *e,
                                    uint16_t soc_centi_pct)
{
    if (e == NULL || soc_centi_pct > 10000U) {
        return -EINVAL;
    }

    int64_t full_uAh = full_capacity_uAh(e);
    if (full_uAh <= 0) {
        return -EINVAL;
    }

    e->status.remaining_charge_uAh =
        (full_uAh * soc_centi_pct) / 10000LL;

    /* Re-anchor so we do not integrate across the calibration operation. */
    e->have_time_anchor = false;
    e->charge_remainder_scaled = 0;

    recompute_percentages(e);
    return 0;
}

int bms_estimator_set_full_capacity_mAh(struct bms_estimator *e,
                                       uint32_t full_capacity_mAh)
{
    if (e == NULL || full_capacity_mAh == 0U) {
        return -EINVAL;
    }

    uint16_t old_soc = e->status.soc_centi_pct;

    e->status.estimated_full_capacity_mAh = full_capacity_mAh;

    int64_t full_uAh = full_capacity_uAh(e);
    e->status.remaining_charge_uAh =
        (full_uAh * old_soc) / 10000LL;

    e->have_time_anchor = false;
    e->charge_remainder_scaled = 0;

    recompute_percentages(e);
    return 0;
}

int bms_estimator_reset_counters(struct bms_estimator *e)
{
    if (e == NULL) {
        return -EINVAL;
    }

    e->status.charged_uAh = 0U;
    e->status.discharged_uAh = 0U;
    e->status.throughput_uAh = 0U;
    e->status.energy_in_uWh = 0U;
    e->status.energy_out_uWh = 0U;
    e->status.equivalent_full_cycles_milli = 0U;
    e->status.skipped_intervals = 0U;

    e->charge_in_remainder = 0U;
    e->charge_out_remainder = 0U;
    e->throughput_remainder = 0U;
    e->energy_in_remainder = 0U;
    e->energy_out_remainder = 0U;

    return 0;
}


int bms_estimator_export_persisted_state(
    const struct bms_estimator *e,
    struct bms_estimator_persisted_state *out)
{
    if (e == NULL || out == NULL || !e->status.valid) {
        return -EINVAL;
    }

    out->remaining_charge_uAh = e->status.remaining_charge_uAh;
    out->estimated_full_capacity_mAh =
        e->status.estimated_full_capacity_mAh;

    out->charged_uAh = e->status.charged_uAh;
    out->discharged_uAh = e->status.discharged_uAh;
    out->throughput_uAh = e->status.throughput_uAh;

    out->energy_in_uWh = e->status.energy_in_uWh;
    out->energy_out_uWh = e->status.energy_out_uWh;

    out->skipped_intervals = e->status.skipped_intervals;

    return 0;
}

int bms_estimator_import_persisted_state(
    struct bms_estimator *e,
    const struct bms_estimator_persisted_state *in)
{
    if (e == NULL || in == NULL ||
        in->estimated_full_capacity_mAh == 0U) {
        return -EINVAL;
    }

    e->status.estimated_full_capacity_mAh =
        in->estimated_full_capacity_mAh;

    int64_t full_uAh = full_capacity_uAh(e);

    if (in->remaining_charge_uAh < 0 ||
        in->remaining_charge_uAh > full_uAh) {
        return -ERANGE;
    }

    e->status.remaining_charge_uAh = in->remaining_charge_uAh;

    e->status.charged_uAh = in->charged_uAh;
    e->status.discharged_uAh = in->discharged_uAh;
    e->status.throughput_uAh = in->throughput_uAh;

    e->status.energy_in_uWh = in->energy_in_uWh;
    e->status.energy_out_uWh = in->energy_out_uWh;

    e->status.skipped_intervals = in->skipped_intervals;

    /*
     * Timing state is not persisted. The first fresh snapshot after restore
     * becomes a new integration anchor.
     */
    e->have_time_anchor = false;
    e->last_timestamp_ms = 0;
    e->charge_remainder_scaled = 0;
    e->charge_in_remainder = 0U;
    e->charge_out_remainder = 0U;
    e->throughput_remainder = 0U;
    e->energy_in_remainder = 0U;
    e->energy_out_remainder = 0U;

    recompute_percentages(e);
    return 0;
}
