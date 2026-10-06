#include <assert.h>
#include <stdio.h>

#include "bms/bms_system_policy.h"

int main(void)
{
    assert(bms_system_classify_health(
        0U, true, false, false) == BMS_SYSTEM_READY);

    assert(bms_system_classify_health(
        BMS_HEALTH_COMM_ERROR,
        true, false, false) == BMS_SYSTEM_DEGRADED);

    assert(bms_system_classify_health(
        BMS_HEALTH_MEASUREMENT_STALE,
        true, false, false) == BMS_SYSTEM_SAFE_LATCHED);

    assert(bms_system_classify_health(
        0U, true, true, false) == BMS_SYSTEM_MAINTENANCE);

    assert(bms_system_classify_health(
        0U, true, false, true) == BMS_SYSTEM_SAFE_LATCHED);

    assert((bms_system_critical_health_mask() &
            BMS_HEALTH_RUNTIME_ERROR) != 0U);

    assert((bms_system_warning_health_mask() &
            BMS_HEALTH_COMM_ERROR) != 0U);

    puts("PASS: system health policy host tests");
    return 0;
}
