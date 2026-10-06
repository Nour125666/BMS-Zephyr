#ifndef BMS_TOPOLOGY_H_
#define BMS_TOPOLOGY_H_

#include <stdint.h>
#include "bms_monitor.h"
#include "bms_snapshot.h"

#ifndef CONFIG_BMS_TOPOLOGY_MAX_MONITORS
#define CONFIG_BMS_TOPOLOGY_MAX_MONITORS 4
#endif

struct bms_topology_segment_cfg {
    struct bms_monitor *monitor;
    uint8_t local_cell_offset;
    uint8_t cell_count;
    uint8_t global_cell_offset;
};

struct bms_topology_config {
    const char *name;
    const struct bms_topology_segment_cfg *segments;
    uint8_t segment_count;

    struct bms_monitor *current_provider;
    struct bms_monitor *pack_voltage_provider;
    struct bms_monitor *switch_provider;
};

struct bms_topology {
    const struct bms_topology_config *config;
    uint8_t total_cell_count;
    uint32_t snapshot_sequence;
};

int bms_topology_init(struct bms_topology *, const struct bms_topology_config *);
int bms_topology_read_snapshot(struct bms_topology *, struct bms_snapshot *);
int bms_topology_get_balance_mask(struct bms_topology *, uint64_t *);
int bms_topology_set_balance_mask(struct bms_topology *, uint64_t);
int bms_topology_get_switch_state(struct bms_topology *,
                                  struct bms_monitor_switch_state *);
int bms_topology_set_switch(struct bms_topology *,
                            enum bms_monitor_switch_request);

#endif
