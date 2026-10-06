#ifndef BMS_PROTECTION_H_
#define BMS_PROTECTION_H_

#include <stdbool.h>
#include <stdint.h>

#include "bms_snapshot.h"

/*
 * These are policy reasons, not raw BQ register bits.
 * Lower bits are software-derived. Upper bits mirror generic monitor faults.
 */
enum bms_protection_reason {
    BMS_PROT_SW_CELL_UV          = (1ULL << 0),
    BMS_PROT_SW_CELL_OV          = (1ULL << 1),
    BMS_PROT_SW_CHARGE_OC        = (1ULL << 2),
    BMS_PROT_SW_DISCHARGE_OC     = (1ULL << 3),
    BMS_PROT_STALE_SNAPSHOT      = (1ULL << 4),

    BMS_PROT_HW_CELL_UV          = (1ULL << 16),
    BMS_PROT_HW_CELL_OV          = (1ULL << 17),
    BMS_PROT_HW_CHARGE_OC        = (1ULL << 18),
    BMS_PROT_HW_DISCHARGE_OC     = (1ULL << 19),
    BMS_PROT_HW_SHORT_CIRCUIT    = (1ULL << 20),
    BMS_PROT_HW_PERMANENT_FAIL   = (1ULL << 21),
};

struct bms_protection_config {
    int32_t cell_uv_trip_mv;
    int32_t cell_uv_release_mv;

    int32_t cell_ov_trip_mv;
    int32_t cell_ov_release_mv;

    /* Positive charging current thresholds. */
    int32_t charge_oc_trip_ma;
    int32_t charge_oc_release_ma;

    /* Positive magnitudes; discharge current itself is negative. */
    int32_t discharge_oc_trip_ma;
    int32_t discharge_oc_release_ma;

    uint32_t max_snapshot_age_ms;
};

struct bms_protection_decision {
    uint32_t snapshot_sequence;
    uint64_t reasons;

    bool charge_allowed;
    bool discharge_allowed;
};

struct bms_protection {
    struct bms_protection_config config;

    /* Stateful software reasons retained for hysteresis. */
    uint64_t active_software_reasons;

    struct bms_protection_decision latest;
};

int bms_protection_init(struct bms_protection *,
                        const struct bms_protection_config *);

int bms_protection_evaluate(struct bms_protection *,
                            const struct bms_snapshot *,
                            int64_t snapshot_age_ms,
                            struct bms_protection_decision *);

const char *bms_protection_reason_name(uint64_t one_reason_bit);

#endif
