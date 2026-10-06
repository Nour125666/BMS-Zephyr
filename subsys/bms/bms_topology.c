#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "bms/bms_topology.h"

static bool member(const struct bms_topology_config *cfg, const struct bms_monitor *m)
{
    if (!m) return false;
    for (uint8_t i = 0; i < cfg->segment_count; ++i)
        if (cfg->segments[i].monitor == m) return true;
    return false;
}

static int provider_ok(const struct bms_topology_config *cfg,
                       struct bms_monitor *m,
                       enum bms_monitor_capability cap)
{
    if (!m) return 0;
    if (!member(cfg, m)) return -EINVAL;
    return bms_monitor_has_capability(m, cap) ? 0 : -ENOTSUP;
}

int bms_topology_init(struct bms_topology *t, const struct bms_topology_config *cfg)
{
    bool used[CONFIG_BMS_MAX_CELLS] = {0};
    uint8_t high = 0;

    if (!t || !cfg || !cfg->segments || cfg->segment_count == 0 ||
        cfg->segment_count > CONFIG_BMS_TOPOLOGY_MAX_MONITORS) return -EINVAL;

    for (uint8_t s = 0; s < cfg->segment_count; ++s) {
        const struct bms_topology_segment_cfg *seg = &cfg->segments[s];
        struct bms_monitor *m = seg->monitor;

        if (!m || !m->ops || !m->backend || seg->cell_count == 0) return -EINVAL;
        if (!bms_monitor_has_capability(m, BMS_MON_CAP_CELL_VOLTAGE)) return -ENOTSUP;
        if ((uint16_t)seg->local_cell_offset + seg->cell_count > m->cell_count) return -ERANGE;
        if ((uint16_t)seg->global_cell_offset + seg->cell_count > CONFIG_BMS_MAX_CELLS) return -E2BIG;

        for (uint8_t c = 0; c < seg->cell_count; ++c) {
            uint8_t g = seg->global_cell_offset + c;
            if (used[g]) return -EEXIST;
            used[g] = true;
            if ((uint8_t)(g + 1U) > high) high = g + 1U;
        }
    }

    for (uint8_t i = 0; i < high; ++i) if (!used[i]) return -EINVAL;

    int ret = provider_ok(cfg, cfg->current_provider, BMS_MON_CAP_CURRENT);
    if (ret) return ret;
    ret = provider_ok(cfg, cfg->pack_voltage_provider, BMS_MON_CAP_PACK_VOLTAGE);
    if (ret) return ret;
    ret = provider_ok(cfg, cfg->switch_provider, BMS_MON_CAP_SWITCH_CONTROL);
    if (ret) return ret;

    memset(t, 0, sizeof(*t));
    t->config = cfg;
    t->total_cell_count = high;
    return 0;
}

int bms_topology_get_balance_mask(struct bms_topology *t, uint64_t *pack_mask)
{
    if (!t || !t->config || !pack_mask) return -EINVAL;
    uint64_t result = 0;

    for (uint8_t s = 0; s < t->config->segment_count; ++s) {
        const struct bms_topology_segment_cfg *seg = &t->config->segments[s];
        struct bms_monitor *m = seg->monitor;

        if (!bms_monitor_has_capability(m, BMS_MON_CAP_BALANCING)) continue;

        uint64_t local = 0;
        int ret = bms_monitor_get_balance_mask(m, &local);
        if (ret) return ret;

        for (uint8_t c = 0; c < seg->cell_count; ++c) {
            uint8_t li = seg->local_cell_offset + c;
            uint8_t gi = seg->global_cell_offset + c;
            if (local & (1ULL << li)) result |= (1ULL << gi);
        }
    }

    *pack_mask = result;
    return 0;
}

int bms_topology_set_balance_mask(struct bms_topology *t, uint64_t pack_mask)
{
    if (!t || !t->config) return -EINVAL;
    if (t->total_cell_count < 64U) {
        uint64_t valid = (1ULL << t->total_cell_count) - 1ULL;
        if (pack_mask & ~valid) return -ERANGE;
    }

    for (uint8_t s = 0; s < t->config->segment_count; ++s) {
        const struct bms_topology_segment_cfg *seg = &t->config->segments[s];
        uint64_t local = 0;
        bool requested = false;

        for (uint8_t c = 0; c < seg->cell_count; ++c) {
            uint8_t li = seg->local_cell_offset + c;
            uint8_t gi = seg->global_cell_offset + c;
            if (pack_mask & (1ULL << gi)) {
                local |= (1ULL << li);
                requested = true;
            }
        }

        if (!bms_monitor_has_capability(seg->monitor, BMS_MON_CAP_BALANCING)) {
            if (requested) return -ENOTSUP;
            continue;
        }

        int ret = bms_monitor_set_balance_mask(seg->monitor, local);
        if (ret) return ret;
    }
    return 0;
}

int bms_topology_get_switch_state(struct bms_topology *t,
                                  struct bms_monitor_switch_state *s)
{
    if (!t || !t->config || !s) return -EINVAL;
    if (!t->config->switch_provider) return -ENOTSUP;
    return bms_monitor_read_switch_state(t->config->switch_provider, s);
}

int bms_topology_set_switch(struct bms_topology *t,
                            enum bms_monitor_switch_request r)
{
    if (!t || !t->config) return -EINVAL;
    if (!t->config->switch_provider) return -ENOTSUP;
    return bms_monitor_set_switch(t->config->switch_provider, r);
}

int bms_topology_read_snapshot(struct bms_topology *t, struct bms_snapshot *snap)
{
    if (!t || !t->config || !snap) return -EINVAL;

    struct bms_monitor_sample locals[CONFIG_BMS_TOPOLOGY_MAX_MONITORS];
    memset(snap, 0, sizeof(*snap));

    snap->sequence = ++t->snapshot_sequence;
    snap->monitor_count = t->config->segment_count;
    snap->cell_count = t->total_cell_count;

    for (uint8_t s = 0; s < t->config->segment_count; ++s) {
        int ret = bms_monitor_read_sample(t->config->segments[s].monitor, &locals[s]);
        if (ret) return ret;
    }

    bool stack_valid = true;
    bool first = true;

    for (uint8_t s = 0; s < t->config->segment_count; ++s) {
        const struct bms_topology_segment_cfg *seg = &t->config->segments[s];
        const struct bms_monitor_sample *local = &locals[s];

        if ((uint16_t)seg->local_cell_offset + seg->cell_count > local->cell_count)
            return -EPROTO;

        for (uint8_t c = 0; c < seg->cell_count; ++c) {
            uint8_t li = seg->local_cell_offset + c;
            uint8_t gi = seg->global_cell_offset + c;
            int32_t mv = local->cell_mv[li];

            snap->cell_mv[gi] = mv;
            snap->cell_sum_mv += mv;

            if (first || mv < snap->min_cell_mv) {
                snap->min_cell_mv = mv;
                snap->min_cell_index = gi;
            }
            if (first || mv > snap->max_cell_mv) {
                snap->max_cell_mv = mv;
                snap->max_cell_index = gi;
            }
            first = false;
        }

        if (local->stack_voltage_valid) snap->stack_mv += local->stack_mv;
        else stack_valid = false;

        if (bms_monitor_has_capability(seg->monitor, BMS_MON_CAP_LOCAL_PROTECTION)) {
            struct bms_monitor_fault_state f;
            int ret = bms_monitor_read_faults(seg->monitor, &f);
            if (ret) return ret;
            snap->alerts |= f.alerts;
            snap->faults |= f.faults;
            snap->permanent_fail |= f.permanent_fail;
        }
    }

    snap->stack_voltage_valid = stack_valid;

    for (uint8_t s = 0; s < t->config->segment_count; ++s) {
        struct bms_monitor *m = t->config->segments[s].monitor;
        const struct bms_monitor_sample *local = &locals[s];

        if (m == t->config->current_provider) {
            if (!local->current_valid) return -ENODATA;
            snap->current_valid = true;
            snap->current_ma = local->current_ma;
        }
        if (m == t->config->pack_voltage_provider) {
            if (!local->pack_voltage_valid) return -ENODATA;
            snap->pack_voltage_valid = true;
            snap->pack_mv = local->pack_mv;
        }
    }

    /*
     * In stacked packs a single AFE may not expose the complete pack voltage.
     * If no explicit pack-voltage provider exists, use the sum of monitor stack
     * voltages as the logical pack voltage.
     */
    if (t->config->pack_voltage_provider == NULL && snap->stack_voltage_valid) {
        snap->pack_voltage_valid = true;
        snap->pack_mv = snap->stack_mv;
    }

    int ret = bms_topology_get_balance_mask(t, &snap->balance_mask);
    if (ret) return ret;

    ret = bms_topology_get_switch_state(t, &snap->switch_state);
    if (ret == 0) snap->switch_state_valid = true;
    else if (ret != -ENOTSUP) return ret;

    return 0;
}
