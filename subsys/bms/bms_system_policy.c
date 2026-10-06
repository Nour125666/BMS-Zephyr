#include "bms/bms_system_policy.h"

uint64_t bms_system_critical_health_mask(void)
{
    return BMS_HEALTH_SELF_TEST_FAILED |
           BMS_HEALTH_NO_SNAPSHOT |
           BMS_HEALTH_MEASUREMENT_STALE |
           BMS_HEALTH_MEASUREMENT_ERROR |
           BMS_HEALTH_MEASUREMENT_STOPPED |
           BMS_HEALTH_MEASUREMENT_NO_PROGRESS |
           BMS_HEALTH_RUNTIME_ERROR |
           BMS_HEALTH_RUNTIME_STOPPED |
           BMS_HEALTH_RUNTIME_NO_PROGRESS;
}

uint64_t bms_system_warning_health_mask(void)
{
    return BMS_HEALTH_COMM_ERROR |
           BMS_HEALTH_COMM_STOPPED |
           BMS_HEALTH_COMM_NO_PROGRESS;
}

enum bms_system_mode bms_system_classify_health(
    uint64_t flags,
    bool self_test_passed,
    bool maintenance_mode,
    bool safe_latched)
{
    if (safe_latched) {
        return BMS_SYSTEM_SAFE_LATCHED;
    }

    if (maintenance_mode) {
        return BMS_SYSTEM_MAINTENANCE;
    }

    if (!self_test_passed ||
        (flags & bms_system_critical_health_mask()) != 0U) {
        return BMS_SYSTEM_SAFE_LATCHED;
    }

    if ((flags & bms_system_warning_health_mask()) != 0U) {
        return BMS_SYSTEM_DEGRADED;
    }

    return BMS_SYSTEM_READY;
}

const char *bms_system_mode_name(enum bms_system_mode mode)
{
    switch (mode) {
    case BMS_SYSTEM_BOOTING:      return "BOOTING";
    case BMS_SYSTEM_READY:        return "READY";
    case BMS_SYSTEM_DEGRADED:     return "DEGRADED";
    case BMS_SYSTEM_MAINTENANCE:  return "MAINTENANCE";
    case BMS_SYSTEM_SAFE_LATCHED: return "SAFE_LATCHED";
    default:                      return "UNKNOWN";
    }
}

const char *bms_system_health_flag_name(uint64_t flag)
{
    switch (flag) {
    case BMS_HEALTH_SELF_TEST_FAILED:
        return "SELF_TEST_FAILED";
    case BMS_HEALTH_NO_SNAPSHOT:
        return "NO_SNAPSHOT";
    case BMS_HEALTH_MEASUREMENT_STALE:
        return "MEASUREMENT_STALE";
    case BMS_HEALTH_MEASUREMENT_ERROR:
        return "MEASUREMENT_ERROR";
    case BMS_HEALTH_MEASUREMENT_STOPPED:
        return "MEASUREMENT_STOPPED";
    case BMS_HEALTH_MEASUREMENT_NO_PROGRESS:
        return "MEASUREMENT_NO_PROGRESS";
    case BMS_HEALTH_RUNTIME_ERROR:
        return "RUNTIME_ERROR";
    case BMS_HEALTH_RUNTIME_STOPPED:
        return "RUNTIME_STOPPED";
    case BMS_HEALTH_RUNTIME_NO_PROGRESS:
        return "RUNTIME_NO_PROGRESS";
    case BMS_HEALTH_COMM_ERROR:
        return "COMM_ERROR";
    case BMS_HEALTH_COMM_STOPPED:
        return "COMM_STOPPED";
    case BMS_HEALTH_COMM_NO_PROGRESS:
        return "COMM_NO_PROGRESS";
    default:
        return "UNKNOWN";
    }
}
