#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "bms/bms_balancing.h"

static bool config_valid(const struct bms_balancing_config *c)
{
    return c != NULL &&
           c->start_cell_mv > 0 &&
           c->start_delta_mv > 0 &&
           c->stop_delta_mv >= 0 &&
           c->stop_delta_mv < c->start_delta_mv &&
           c->max_abs_current_ma >= 0 &&
           c->max_simultaneous_cells > 0U;
}

int bms_balancing_init(struct bms_balancing *b,
                       const struct bms_balancing_config *config)
{
    if (b == NULL || !config_valid(config)) {
        return -EINVAL;
    }

    memset(b, 0, sizeof(*b));
    b->config = *config;
    return 0;
}

int bms_balancing_set_enabled(struct bms_balancing *b, bool enabled)
{
    if (b == NULL) {
        return -EINVAL;
    }

    b->config.enabled = enabled;

    if (!enabled) {
        b->previous_desired_mask = 0U;
        b->latest.desired_mask = 0U;
    }

    return 0;
}

static uint64_t select_highest_cells(const struct bms_snapshot *s,
                                     uint64_t eligible,
                                     uint8_t max_cells)
{
    uint64_t selected = 0U;

    for (uint8_t chosen = 0U; chosen < max_cells; ++chosen) {
        int32_t best_mv = INT32_MIN;
        int best_idx = -1;

        for (uint8_t i = 0U; i < s->cell_count; ++i) {
            uint64_t bit = (1ULL << i);

            if ((eligible & bit) == 0U || (selected & bit) != 0U) {
                continue;
            }

            if (best_idx < 0 || s->cell_mv[i] > best_mv) {
                best_mv = s->cell_mv[i];
                best_idx = i;
            }
        }

        if (best_idx < 0) {
            break;
        }

        selected |= (1ULL << (uint8_t)best_idx);
    }

    return selected;
}

int bms_balancing_evaluate(struct bms_balancing *b,
                           const struct bms_snapshot *s,
                           const struct bms_protection_decision *p,
                           struct bms_balancing_decision *out)
{
    if (b == NULL || s == NULL || p == NULL || out == NULL ||
        s->cell_count == 0U || s->cell_count > 64U) {
        return -EINVAL;
    }

    struct bms_balancing_decision d = {
        .snapshot_sequence = s->sequence,
    };

    if (!b->config.enabled) {
        d.inhibit_reasons |= BMS_BAL_INHIBIT_DISABLED;
    }

    /* Any active protection decision inhibits balancing in this version. */
    if (!p->charge_allowed || !p->discharge_allowed || p->reasons != 0U) {
        d.inhibit_reasons |= BMS_BAL_INHIBIT_PROTECTION;
    }

    if (!s->current_valid) {
        d.inhibit_reasons |= BMS_BAL_INHIBIT_CURRENT_INVALID;
    } else {
        int64_t abs_current = s->current_ma;
        if (abs_current < 0) {
            abs_current = -abs_current;
        }
        if (abs_current > b->config.max_abs_current_ma) {
            d.inhibit_reasons |= BMS_BAL_INHIBIT_CURRENT_HIGH;
        }
    }

    if (s->max_cell_mv < b->config.start_cell_mv) {
        d.inhibit_reasons |= BMS_BAL_INHIBIT_VOLTAGE_LOW;
    }

    if ((s->max_cell_mv - s->min_cell_mv) < b->config.stop_delta_mv) {
        d.inhibit_reasons |= BMS_BAL_INHIBIT_DELTA_SMALL;
    }

    /* Hard inhibits force balancing off immediately. */
    const uint32_t hard_inhibits =
        BMS_BAL_INHIBIT_DISABLED |
        BMS_BAL_INHIBIT_PROTECTION |
        BMS_BAL_INHIBIT_CURRENT_INVALID |
        BMS_BAL_INHIBIT_CURRENT_HIGH |
        BMS_BAL_INHIBIT_VOLTAGE_LOW;

    if ((d.inhibit_reasons & hard_inhibits) == 0U) {
        for (uint8_t i = 0U; i < s->cell_count; ++i) {
            uint64_t bit = (1ULL << i);
            int32_t delta = s->cell_mv[i] - s->min_cell_mv;
            bool was_active = (b->previous_desired_mask & bit) != 0U;

            bool eligible;
            if (was_active) {
                eligible = s->cell_mv[i] >= b->config.start_cell_mv &&
                           delta > b->config.stop_delta_mv;
            } else {
                eligible = s->cell_mv[i] >= b->config.start_cell_mv &&
                           delta >= b->config.start_delta_mv;
            }

            if (eligible) {
                d.eligible_mask |= bit;
            }
        }

        if (d.eligible_mask == 0U) {
            d.inhibit_reasons |= BMS_BAL_INHIBIT_DELTA_SMALL;
        } else {
            d.inhibit_reasons &= ~BMS_BAL_INHIBIT_DELTA_SMALL;
            d.desired_mask = select_highest_cells(
                s, d.eligible_mask, b->config.max_simultaneous_cells);
        }
    }

    b->previous_desired_mask = d.desired_mask;
    b->latest = d;
    *out = d;
    return 0;
}

const char *bms_balancing_inhibit_name(uint32_t bit)
{
    switch (bit) {
    case BMS_BAL_INHIBIT_DISABLED:        return "DISABLED";
    case BMS_BAL_INHIBIT_PROTECTION:      return "PROTECTION_ACTIVE";
    case BMS_BAL_INHIBIT_CURRENT_INVALID: return "CURRENT_INVALID";
    case BMS_BAL_INHIBIT_CURRENT_HIGH:    return "CURRENT_TOO_HIGH";
    case BMS_BAL_INHIBIT_VOLTAGE_LOW:     return "CELL_VOLTAGE_TOO_LOW";
    case BMS_BAL_INHIBIT_DELTA_SMALL:     return "CELL_DELTA_TOO_SMALL";
    default:                               return "UNKNOWN";
    }
}
