#ifndef BMS_CONFIG_H_
#define BMS_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#include "bms_balancing.h"
#include "bms_estimator.h"
#include "bms_protection.h"

#define BMS_CONFIG_SCHEMA_VERSION 1U

/*
 * Product/runtime configuration.
 *
 * This is separate from:
 *   - hardware topology (which monitor maps to which cells)
 *   - the latest battery snapshot
 *   - retained estimator counters/state
 */
struct bms_app_config {
    uint16_t schema_version;

    uint32_t measurement_period_ms;
    uint32_t runtime_period_ms;

    struct bms_protection_config protection;
    struct bms_balancing_config balancing;
    struct bms_estimator_config estimator;
};

struct bms_persistent_data {
    struct bms_app_config config;

    bool estimator_state_valid;
    struct bms_estimator_persisted_state estimator_state;
};

void bms_config_defaults(struct bms_app_config *);

int bms_config_validate(const struct bms_app_config *);

bool bms_config_equal(const struct bms_app_config *,
                      const struct bms_app_config *);

#endif
