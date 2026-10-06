#include <errno.h>
#include <string.h>

#include "bms/bms_config.h"

void bms_config_defaults(struct bms_app_config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->schema_version = BMS_CONFIG_SCHEMA_VERSION;

    cfg->measurement_period_ms = 200U;
    cfg->runtime_period_ms = 100U;

    /*
     * DEVELOPMENT / SIMULATION DEFAULTS.
     * These remain intentionally separate from any final battery product
     * specification.
     */
    cfg->protection.cell_uv_trip_mv = 2800;
    cfg->protection.cell_uv_release_mv = 3000;

    cfg->protection.cell_ov_trip_mv = 3650;
    cfg->protection.cell_ov_release_mv = 3500;

    cfg->protection.charge_oc_trip_ma = 30000;
    cfg->protection.charge_oc_release_ma = 25000;

    cfg->protection.discharge_oc_trip_ma = 50000;
    cfg->protection.discharge_oc_release_ma = 40000;

    cfg->protection.max_snapshot_age_ms = 1000U;

    cfg->balancing.enabled = true;
    cfg->balancing.start_cell_mv = 3400;
    cfg->balancing.start_delta_mv = 20;
    cfg->balancing.stop_delta_mv = 10;
    cfg->balancing.max_abs_current_ma = 5000;
    cfg->balancing.max_simultaneous_cells = 4U;

    cfg->estimator.design_capacity_mAh = 100000U;
    cfg->estimator.initial_full_capacity_mAh = 100000U;
    cfg->estimator.initial_soc_centi_pct = 5000U;
    cfg->estimator.charge_efficiency_permille = 1000U;
    cfg->estimator.max_integration_interval_ms = 1000U;
}

static bool protection_valid(const struct bms_protection_config *c)
{
    return c->cell_uv_trip_mv > 0 &&
           c->cell_uv_release_mv > c->cell_uv_trip_mv &&
           c->cell_ov_trip_mv > c->cell_ov_release_mv &&
           c->cell_ov_release_mv > c->cell_uv_release_mv &&
           c->charge_oc_trip_ma > c->charge_oc_release_ma &&
           c->charge_oc_release_ma >= 0 &&
           c->discharge_oc_trip_ma > c->discharge_oc_release_ma &&
           c->discharge_oc_release_ma >= 0 &&
           c->max_snapshot_age_ms > 0U;
}

static bool balancing_valid(const struct bms_balancing_config *c)
{
    return c->start_cell_mv > 0 &&
           c->start_delta_mv > 0 &&
           c->stop_delta_mv >= 0 &&
           c->start_delta_mv > c->stop_delta_mv &&
           c->max_abs_current_ma >= 0 &&
           c->max_simultaneous_cells > 0U &&
           c->max_simultaneous_cells <= 32U;
}

static bool estimator_valid(const struct bms_estimator_config *c)
{
    return c->design_capacity_mAh > 0U &&
           c->initial_full_capacity_mAh > 0U &&
           c->initial_soc_centi_pct <= 10000U &&
           c->charge_efficiency_permille > 0U &&
           c->charge_efficiency_permille <= 1000U &&
           c->max_integration_interval_ms > 0U;
}

int bms_config_validate(const struct bms_app_config *cfg)
{
    if (cfg == NULL) {
        return -EINVAL;
    }

    if (cfg->schema_version != BMS_CONFIG_SCHEMA_VERSION) {
        return -EPROTONOSUPPORT;
    }

    if (cfg->measurement_period_ms == 0U ||
        cfg->runtime_period_ms == 0U) {
        return -EINVAL;
    }

    if (!protection_valid(&cfg->protection) ||
        !balancing_valid(&cfg->balancing) ||
        !estimator_valid(&cfg->estimator)) {
        return -EINVAL;
    }

    /*
     * Protection stale timeout should be comfortably larger than the
     * measurement cadence so one late cycle does not instantly trip stale.
     */
    if (cfg->protection.max_snapshot_age_ms <
        cfg->measurement_period_ms * 2U) {
        return -ERANGE;
    }

    return 0;
}

bool bms_config_equal(const struct bms_app_config *a,
                      const struct bms_app_config *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return memcmp(a, b, sizeof(*a)) == 0;
}
