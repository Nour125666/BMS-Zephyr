#ifndef BMS_SYSTEM_POLICY_H_
#define BMS_SYSTEM_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

enum bms_system_mode {
    BMS_SYSTEM_BOOTING = 0,
    BMS_SYSTEM_READY,
    BMS_SYSTEM_DEGRADED,
    BMS_SYSTEM_MAINTENANCE,
    BMS_SYSTEM_SAFE_LATCHED,
};

enum bms_system_health_flag {
    BMS_HEALTH_SELF_TEST_FAILED       = (1ULL << 0),
    BMS_HEALTH_NO_SNAPSHOT            = (1ULL << 1),
    BMS_HEALTH_MEASUREMENT_STALE      = (1ULL << 2),
    BMS_HEALTH_MEASUREMENT_ERROR      = (1ULL << 3),
    BMS_HEALTH_MEASUREMENT_STOPPED    = (1ULL << 4),
    BMS_HEALTH_MEASUREMENT_NO_PROGRESS= (1ULL << 5),

    BMS_HEALTH_RUNTIME_ERROR          = (1ULL << 8),
    BMS_HEALTH_RUNTIME_STOPPED        = (1ULL << 9),
    BMS_HEALTH_RUNTIME_NO_PROGRESS    = (1ULL << 10),

    BMS_HEALTH_COMM_ERROR             = (1ULL << 16),
    BMS_HEALTH_COMM_STOPPED           = (1ULL << 17),
    BMS_HEALTH_COMM_NO_PROGRESS       = (1ULL << 18),
};

uint64_t bms_system_critical_health_mask(void);
uint64_t bms_system_warning_health_mask(void);

enum bms_system_mode bms_system_classify_health(
    uint64_t flags,
    bool self_test_passed,
    bool maintenance_mode,
    bool safe_latched);

const char *bms_system_mode_name(enum bms_system_mode mode);
const char *bms_system_health_flag_name(uint64_t one_flag);

#endif
