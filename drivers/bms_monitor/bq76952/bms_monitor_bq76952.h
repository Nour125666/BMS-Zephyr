#ifndef BMS_MONITOR_BQ76952_H_
#define BMS_MONITOR_BQ76952_H_

#include "bms/bms_monitor.h"
#include "bq76952.h"

struct bms_monitor_bq76952 {
    struct bms_monitor monitor;
    struct bq76952_device bq;
};

int bms_monitor_bq76952_init(struct bms_monitor_bq76952 *,
                             const char *, uint8_t instance_id);

#endif
