#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "bms/bms_core_runtime.h"
#include "bms/bms_config.h"
#include "bms/bms_config_store.h"
#include "bms/bms_communications.h"
#include "bms/bms_communications_virtual.h"
#include "bms/bms_system.h"
#include "bms/bms_watchdog.h"
#include "bms/bms_measurement.h"
#include "bms/bms_topology.h"
#include "bms_monitor_bq76952.h"
#include "bq76952_reg.h"
#include "virtual_bq_model.h"

static struct bms_monitor_bq76952 bq0, bq1;
static struct bms_topology topo;
static struct bms_measurement measurement;
static struct bms_core_runtime runtime;
static struct bms_topology_segment_cfg segs[2];
static struct bms_topology_config cfg;

/*
 * Staged configuration can be edited from the shell without immediately
 * disturbing the running BMS. `bms config apply` validates it and applies it
 * in a controlled stop/reconfigure/restart sequence.
 */
static struct bms_app_config staged_config;
static struct bms_app_config applied_config;

static struct bms_config_store config_store;
static struct bms_config_store_virtual virtual_nv;

static struct bms_communications communications;
static struct bms_virtual_can_backend virtual_can;
static struct bms_virtual_rs485_backend virtual_rs485;
static uint16_t virtual_request_sequence = 1U;

static struct bms_system system_supervisor;
static struct bms_virtual_watchdog virtual_watchdog;

static struct bms_monitor_bq76952 *get_bq(unsigned long i)
{
    if (i == 0U) return &bq0;
    if (i == 1U) return &bq1;
    return NULL;
}

static int t_transport(const struct shell *sh)
{
    for (uint8_t i = 0; i < 2; ++i) {
        struct bms_monitor_bq76952 *x = get_bq(i);
        int16_t mv;
        int ret = bq76952_direct_read_i16(&x->bq.transport, BQ_CMD_CELL1, &mv);
        if (ret) return ret;

        uint8_t raw[2]; size_t n = 0;
        ret = bq76952_subcmd_read(&x->bq.transport, BQ_SUBCMD_DEVICE_NUMBER,
                                  raw, sizeof(raw), &n);
        if (ret || n != 2U) return ret ? ret : -EPROTO;

        uint8_t da = 0;
        ret = bq76952_dm_read(&x->bq.transport, BQ_DM_DA_CONFIGURATION, &da, 1U);
        if (ret || da != 0x06U) return ret ? ret : -EPROTO;

        shell_print(sh, "[PASS] transport-contract BQ%u", i);
    }
    return 0;
}

static int t_driver(const struct shell *sh)
{
    for (uint8_t i = 0; i < 2; ++i) {
        struct bms_monitor_bq76952 *x = get_bq(i);
        struct bq76952_measurements m;
        struct bq76952_faults f;
        struct bq76952_fet_state s;

        int ret = bq76952_read_all_measurements(&x->bq, &m); if (ret) return ret;
        ret = bq76952_read_faults(&x->bq, &f); if (ret) return ret;
        ret = bq76952_read_fet_state(&x->bq, &s); if (ret) return ret;

        shell_print(sh, "[PASS] semantic-driver BQ%u", i);
    }
    return 0;
}

static int t_monitor(const struct shell *sh)
{
    for (uint8_t i = 0; i < 2; ++i) {
        struct bms_monitor_bq76952 *x = get_bq(i);
        struct bms_monitor_sample s;
        struct bms_monitor_fault_state f;

        int ret = bms_monitor_read_sample(&x->monitor, &s); if (ret) return ret;
        ret = bms_monitor_read_faults(&x->monitor, &f); if (ret) return ret;

        shell_print(sh, "[PASS] generic-monitor BQ%u", i);
    }
    return 0;
}

static int t_topology(const struct shell *sh)
{
    struct bms_snapshot s;
    int ret = bms_topology_read_snapshot(&topo, &s);
    if (ret) return ret;

    if (s.cell_count != 32U ||
        s.cell_mv[0] != virtual_bq_get(0)->cell_mv[0] ||
        s.cell_mv[16] != virtual_bq_get(1)->cell_mv[0]) {
        return -EPROTO;
    }

    if (!s.pack_voltage_valid || !s.stack_voltage_valid ||
        s.pack_mv != s.stack_mv || s.pack_mv <= 0) {
        return -EPROTO;
    }

    shell_print(sh, "[PASS] topology + snapshot");
    return 0;
}

static int t_measurement(const struct shell *sh)
{
    struct bms_snapshot s;
    int ret = bms_measurement_refresh_now(&measurement);
    if (ret) return ret;

    ret = bms_measurement_get_latest(&measurement, &s);
    if (ret) return ret;

    if (s.cell_count != 32U || s.captured_at_ms <= 0) {
        return -EPROTO;
    }

    shell_print(sh, "[PASS] measurement + cached snapshot publication");
    return 0;
}

static int t_core_runtime(const struct shell *sh)
{
    int ret = bms_measurement_refresh_now(&measurement);
    if (ret) return ret;

    ret = bms_core_runtime_process_now(&runtime);
    if (ret) return ret;

    struct bms_protection_decision d;
    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret) return ret;

    shell_print(sh, "[PASS] protection decision: CHG=%u DSG=%u reasons=0x%016" PRIx64,
                d.charge_allowed, d.discharge_allowed, d.reasons);
    shell_print(sh, "[PASS] state machine: %s",
                bms_state_name(runtime.state_machine.state));
    return 0;
}

static int t_balancing(const struct shell *sh)
{
    int ret = bms_measurement_refresh_now(&measurement);
    if (ret) return ret;

    ret = bms_core_runtime_process_now(&runtime);
    if (ret) return ret;

    struct bms_balancing_decision d;
    ret = bms_core_runtime_get_balancing_decision(&runtime, &d);
    if (ret) return ret;

    shell_print(sh, "[PASS] balancing policy: desired=0x%08" PRIx64
                    " eligible=0x%08" PRIx64 " inhibit=0x%08" PRIx32,
                d.desired_mask, d.eligible_mask, d.inhibit_reasons);
    return 0;
}

static int t_estimator(const struct shell *sh)
{
    int ret = bms_measurement_refresh_now(&measurement);
    if (ret) return ret;

    ret = bms_core_runtime_process_now(&runtime);
    if (ret) return ret;

    struct bms_estimator_status s;
    ret = bms_core_runtime_get_estimator_status(&runtime, &s);
    if (ret) return ret;

    if (!s.valid || s.soc_centi_pct > 10000U ||
        s.design_capacity_mAh == 0U ||
        s.estimated_full_capacity_mAh == 0U) {
        return -EPROTO;
    }

    shell_print(sh,
                "[PASS] estimator: SOC=%u.%02u%% SOH(capacity)=%u.%02u%%",
                s.soc_centi_pct / 100U,
                s.soc_centi_pct % 100U,
                s.soh_centi_pct / 100U,
                s.soh_centi_pct % 100U);
    return 0;
}

static int t_persistence(const struct shell *sh)
{
    struct bms_persistent_data before;
    int ret = capture_persistent_data(&before);
    if (ret) return ret;

    ret = bms_config_store_save(&config_store, &before);
    if (ret) return ret;

    struct bms_persistent_data after;
    ret = bms_config_store_load(&config_store, &after);
    if (ret) return ret;

    if (!bms_config_equal(&before.config, &after.config) ||
        before.estimator_state_valid != after.estimator_state_valid) {
        return -EPROTO;
    }

    if (before.estimator_state_valid &&
        memcmp(&before.estimator_state,
               &after.estimator_state,
               sizeof(before.estimator_state)) != 0) {
        return -EPROTO;
    }

    shell_print(sh,
                "[PASS] config persistence: version + CRC + round-trip generation=%" PRIu32,
                bms_config_store_last_generation(&config_store));
    return 0;
}

static int t_communications(const struct shell *sh)
{
    size_t before = virtual_can.count;

    int ret = bms_communications_publish_can_now(&communications);
    if (ret != 0) return ret;

    if (virtual_can.count <= before && virtual_can.total_sent == 0U) {
        return -EPROTO;
    }

    ret = send_virtual_rs485_request(BMS_RS485_REQ_STATUS,
                                     NULL,
                                     0U);
    if (ret != 0) return ret;

    struct bms_rs485_frame response;
    ret = bms_protocol_rs485_decode(
        virtual_rs485.last_frame,
        virtual_rs485.last_len,
        &response);
    if (ret != 0) return ret;

    if (response.type != BMS_RS485_RSP_STATUS) {
        return -EPROTO;
    }

    shell_print(sh,
                "[PASS] communications: CAN publish + RS485 status request/response");
    return 0;
}

static int settle_manual(void)
{
    int ret = bms_measurement_refresh_now(&measurement);
    if (ret != 0) return ret;

    return bms_core_runtime_process_now(&runtime);
}

static int t_system(const struct shell *sh)
{
    int ret = bms_system_run_self_test(&system_supervisor);
    if (ret != 0) return ret;

    ret = bms_system_check_now(&system_supervisor);
    if (ret != 0) return ret;

    struct bms_system_health h;
    ret = bms_system_get_health(&system_supervisor, &h);
    if (ret != 0) return ret;

    if (!h.self_test_passed ||
        h.mode == BMS_SYSTEM_SAFE_LATCHED) {
        return -EPROTO;
    }

    shell_print(sh,
                "[PASS] system supervisor: self-test + health + watchdog policy");
    return 0;
}

static int run_regression_scenarios(const struct shell *sh)
{
    /*
     * Stop periodic loops and supervisor so scenarios are deterministic.
     * This is a test harness operation, not normal BMS runtime behavior.
     */
    bool sys_running = atomic_get(&system_supervisor.running) != 0;
    bool meas_running = atomic_get(&measurement.running) != 0;
    bool rt_running = atomic_get(&runtime.running) != 0;
    bool comm_running = atomic_get(&communications.running) != 0;

    if (sys_running) (void)bms_system_stop(&system_supervisor);
    if (comm_running) (void)bms_communications_stop(&communications);
    if (rt_running) (void)bms_core_runtime_stop(&runtime);
    if (meas_running) (void)bms_measurement_stop(&measurement);

    (void)bms_system_set_maintenance(&system_supervisor, true);

    int ret = 0;
    struct bms_protection_decision d;

    /* Baseline. */
    virtual_bq_reset();
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 ||
        !d.charge_allowed ||
        !d.discharge_allowed) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario NORMAL");

    /* Software UV -> discharge blocked. */
    virtual_bq_set_cell(1U, 0U,
        (int16_t)(applied_config.protection.cell_uv_trip_mv - 100));
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 || d.discharge_allowed ||
        (d.reasons & BMS_PROT_SW_CELL_UV) == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario SOFTWARE_UV");

    virtual_bq_set_cell(1U, 0U,
        (int16_t)(applied_config.protection.cell_uv_release_mv + 100));
    ret = settle_manual();
    if (ret != 0) goto out;

    /* Software OV -> charge blocked. */
    virtual_bq_set_cell(0U, 0U,
        (int16_t)(applied_config.protection.cell_ov_trip_mv + 100));
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 || d.charge_allowed ||
        (d.reasons & BMS_PROT_SW_CELL_OV) == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario SOFTWARE_OV");

    virtual_bq_set_cell(0U, 0U, 3300);
    ret = settle_manual();
    if (ret != 0) goto out;

    /* Discharge OC. */
    virtual_bq_set_current(
        0U,
        -(applied_config.protection.discharge_oc_trip_ma + 5000));
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 || d.discharge_allowed ||
        (d.reasons & BMS_PROT_SW_DISCHARGE_OC) == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario DISCHARGE_OC");

    virtual_bq_set_current(0U, 0);
    ret = settle_manual();
    if (ret != 0) goto out;

    /* Charge OC. */
    virtual_bq_set_current(
        0U,
        applied_config.protection.charge_oc_trip_ma + 5000);
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 || d.charge_allowed ||
        (d.reasons & BMS_PROT_SW_CHARGE_OC) == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario CHARGE_OC");

    virtual_bq_set_current(0U, 0);
    ret = settle_manual();
    if (ret != 0) goto out;

    /* Hardware BQ CUV fault path. */
    virtual_bq_set_uv(1U, true);
    ret = settle_manual();
    if (ret != 0) goto out;

    ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret != 0 ||
        (d.reasons & BMS_PROT_HW_CELL_UV) == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario HARDWARE_UV");

    virtual_bq_set_uv(1U, false);
    ret = settle_manual();
    if (ret != 0) goto out;

    /* Balancing policy. */
    virtual_bq_set_cell(0U, 0U,
        (int16_t)(applied_config.balancing.start_cell_mv + 100));
    virtual_bq_set_cell(0U, 1U,
        (int16_t)(applied_config.balancing.start_cell_mv - 100));
    ret = settle_manual();
    if (ret != 0) goto out;

    struct bms_balancing_decision balancing;
    ret = bms_core_runtime_get_balancing_decision(&runtime,
                                                   &balancing);
    if (ret != 0 || balancing.desired_mask == 0U) {
        ret = -EPROTO;
        goto out;
    }
    shell_print(sh, "[PASS] scenario BALANCING");

    /* Communications still encode coherent current state. */
    ret = bms_communications_publish_can_now(&communications);
    if (ret != 0) goto out;

    ret = send_virtual_rs485_request(BMS_RS485_REQ_STATUS,
                                     NULL,
                                     0U);
    if (ret != 0) goto out;

    shell_print(sh, "[PASS] scenario COMMUNICATIONS");

out:
    virtual_bq_reset();

    /* Return to a clean state before re-enabling normal periodic services. */
    (void)bms_measurement_refresh_now(&measurement);
    (void)bms_core_runtime_process_now(&runtime);

    (void)bms_system_set_maintenance(&system_supervisor, false);

    if (meas_running) (void)bms_measurement_start(&measurement);
    if (rt_running) (void)bms_core_runtime_start(&runtime);
    if (comm_running) (void)bms_communications_start(&communications);
    if (sys_running) (void)bms_system_start(&system_supervisor);

    return ret;
}

static int cmd_scenario_all(const struct shell *sh,
                            size_t argc,
                            char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = run_regression_scenarios(sh);

    if (ret == 0) {
        shell_print(sh, "[PASS] ALL REGRESSION SCENARIOS");
    } else {
        shell_error(sh, "[FAIL] regression scenarios -> %d", ret);
    }

    return ret;
}

static int cmd_test_all(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = t_transport(sh); if (ret) return ret;
    ret = t_driver(sh); if (ret) return ret;
    ret = t_monitor(sh); if (ret) return ret;
    ret = t_topology(sh); if (ret) return ret;
    ret = t_measurement(sh); if (ret) return ret;
    ret = t_core_runtime(sh); if (ret) return ret;
    ret = t_balancing(sh); if (ret) return ret;
    ret = t_estimator(sh); if (ret) return ret;
    ret = t_persistence(sh); if (ret) return ret;
    ret = t_communications(sh); if (ret) return ret;
    ret = t_system(sh); if (ret) return ret;

    shell_print(sh, "[PASS] FULL VIRTUAL BMS PLATFORM");
    return 0;
}

static int cmd_test_transport(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_transport(sh); }
static int cmd_test_driver(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_driver(sh); }
static int cmd_test_monitor(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_monitor(sh); }
static int cmd_test_topology(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_topology(sh); }
static int cmd_test_measurement(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_measurement(sh); }
static int cmd_test_runtime(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_core_runtime(sh); }
static int cmd_test_balancing(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_balancing(sh); }

static int cmd_test_estimator(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_estimator(sh); }

static int cmd_test_persistence(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_persistence(sh); }

static int cmd_test_communications(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_communications(sh); }

static int cmd_test_system(const struct shell *sh, size_t a, char **v)
{ ARG_UNUSED(a); ARG_UNUSED(v); return t_system(sh); }

static int cmd_snapshot(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_snapshot s;
    int ret = bms_measurement_get_latest(&measurement, &s);
    if (ret) {
        shell_error(sh, "no published snapshot yet: %d", ret);
        return ret;
    }

    shell_print(sh, "seq=%" PRIu32 " age=%" PRId64 "ms cells=%u current=%" PRId32
                    "mA stack=%" PRId32 "mV pack=%" PRId32 "mV",
                s.sequence, bms_measurement_snapshot_age_ms(&measurement),
                s.cell_count, s.current_ma, s.stack_mv, s.pack_mv);

    shell_print(sh, "min=cell%u/%" PRId32 "mV max=cell%u/%" PRId32
                    "mV faults=0x%016" PRIx64 " balance=0x%08" PRIx64,
                s.min_cell_index + 1U, s.min_cell_mv,
                s.max_cell_index + 1U, s.max_cell_mv,
                s.faults, s.balance_mask);

    if (s.switch_state_valid) {
        shell_print(sh, "switch: CHG=%u DSG=%u",
                    s.switch_state.charge_enabled,
                    s.switch_state.discharge_enabled);
    }

    return 0;
}

static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2U) return -EINVAL;

    unsigned long idx = strtoul(argv[1], NULL, 0);
    struct bms_monitor_bq76952 *x = get_bq(idx);
    if (!x) return -EINVAL;

    struct bq76952_measurements m;
    int ret = bq76952_read_all_measurements(&x->bq, &m);
    if (ret) return ret;

    shell_print(sh, "BQ%lu driver: stack=%" PRId32 "mV pack=%" PRId32
                    "mV current=%" PRId32 "mA",
                idx, m.stack_mv, m.pack_mv, m.current_ma);
    return 0;
}

static int cmd_inject_cell(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 4U) return -EINVAL;

    unsigned long b = strtoul(argv[1], NULL, 0);
    unsigned long c = strtoul(argv[2], NULL, 0);
    long mv = strtol(argv[3], NULL, 0);

    if (b > 1U || c < 1U || c > 16U) return -EINVAL;

    int ret = virtual_bq_set_cell((uint8_t)b,
                                  (uint8_t)(c - 1U),
                                  (int16_t)mv);
    if (!ret) shell_print(sh, "virtual BQ%lu cell%lu -> %ldmV", b, c, mv);
    return ret;
}

static int cmd_inject_current(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3U) return -EINVAL;

    unsigned long b = strtoul(argv[1], NULL, 0);
    long ma = strtol(argv[2], NULL, 0);

    if (b > 1U) return -EINVAL;
    return virtual_bq_set_current((uint8_t)b, (int32_t)ma);
}

static int cmd_inject_uv(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3U) return -EINVAL;

    unsigned long b = strtoul(argv[1], NULL, 0);
    if (b > 1U) return -EINVAL;

    bool on = strcmp(argv[2], "on") == 0;
    if (!on && strcmp(argv[2], "off") != 0) return -EINVAL;

    return virtual_bq_set_uv((uint8_t)b, on);
}

static int cmd_balance(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2U) return -EINVAL;

    uint64_t mask = strtoull(argv[1], NULL, 0);
    shell_warn(sh, "manual balance command bypasses policy; runtime may overwrite it");
    int ret = bms_topology_set_balance_mask(&topo, mask);

    if (!ret) shell_print(sh, "pack balance -> 0x%08" PRIx64, mask);
    return ret;
}

static int cmd_switch(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2U) return -EINVAL;

    enum bms_monitor_switch_request r;

    if (strcmp(argv[1], "release") == 0) r = BMS_SWITCH_RELEASE;
    else if (strcmp(argv[1], "all_off") == 0) r = BMS_SWITCH_ALL_OFF;
    else if (strcmp(argv[1], "charge_off") == 0) r = BMS_SWITCH_CHARGE_OFF;
    else if (strcmp(argv[1], "discharge_off") == 0) r = BMS_SWITCH_DISCHARGE_OFF;
    else return -EINVAL;

    shell_warn(sh, "manual switch command bypasses the runtime decision layer");
    return bms_topology_set_switch(&topo, r);
}


/* ----- Configuration / persistence helpers ----- */

static int apply_staged_config(bool preserve_estimator_state)
{
    int ret = bms_config_validate(&staged_config);
    if (ret != 0) {
        return ret;
    }

    bool supervisor_ready = system_supervisor.initialized;
    if (supervisor_ready) {
        (void)bms_system_set_maintenance(&system_supervisor, true);
    }

    bool measurement_was_running =
        atomic_get(&measurement.running) != 0;
    bool runtime_was_running =
        atomic_get(&runtime.running) != 0;

    if (runtime_was_running) {
        ret = bms_core_runtime_stop(&runtime);
        if (ret != 0) return ret;
    }

    if (measurement_was_running) {
        ret = bms_measurement_stop(&measurement);
        if (ret != 0) return ret;
    }

    ret = bms_measurement_set_period(
        &measurement, staged_config.measurement_period_ms);

    if (ret == 0) {
        ret = bms_core_runtime_apply_configs(
            &runtime,
            &staged_config.protection,
            &staged_config.balancing,
            &staged_config.estimator,
            preserve_estimator_state);
    }

    if (ret == 0) {
        ret = bms_core_runtime_set_period(
            &runtime, staged_config.runtime_period_ms);
    }

    if (ret == 0) {
        applied_config = staged_config;
    }

    /*
     * Restart only after the full reconfiguration succeeded.
     * If it failed, leave the services stopped so the failure is obvious.
     */
    if (ret == 0 && measurement_was_running) {
        ret = bms_measurement_start(&measurement);
    }

    if (ret == 0 && runtime_was_running) {
        ret = bms_core_runtime_start(&runtime);
    }

    if (supervisor_ready) {
        (void)bms_system_set_maintenance(&system_supervisor, false);
    }

    return ret;
}

static int capture_persistent_data(struct bms_persistent_data *data)
{
    if (data == NULL) return -EINVAL;

    memset(data, 0, sizeof(*data));
    data->config = applied_config;

    int ret = bms_core_runtime_export_estimator_persisted_state(
        &runtime, &data->estimator_state);

    if (ret == 0) {
        data->estimator_state_valid = true;
        return 0;
    }

    /* Config is still saveable even if estimator has no valid state yet. */
    data->estimator_state_valid = false;
    memset(&data->estimator_state, 0, sizeof(data->estimator_state));
    return 0;
}

static int restore_persistent_data(const struct bms_persistent_data *data)
{
    if (data == NULL) return -EINVAL;

    staged_config = data->config;

    /*
     * Simulate boot: rebuild estimator from config, then restore retained
     * counters/SOC. We intentionally do NOT preserve the pre-"reboot" RAM
     * estimator state.
     */
    int ret = apply_staged_config(false);
    if (ret != 0) return ret;

    if (data->estimator_state_valid) {
        ret = bms_core_runtime_import_estimator_persisted_state(
            &runtime, &data->estimator_state);
    }

    return ret;
}


/* ----- Communications config callbacks ----- */

static int comm_get_applied_config(void *context,
                                   struct bms_app_config *out)
{
    ARG_UNUSED(context);

    if (out == NULL) return -EINVAL;
    *out = applied_config;
    return 0;
}

static int comm_stage_config(void *context,
                             const struct bms_app_config *cfg_in)
{
    ARG_UNUSED(context);

    if (cfg_in == NULL) return -EINVAL;

    int ret = bms_config_validate(cfg_in);
    if (ret != 0) return ret;

    staged_config = *cfg_in;
    return 0;
}

static int comm_apply_config(void *context)
{
    ARG_UNUSED(context);
    return apply_staged_config(true);
}

static int comm_save_config(void *context)
{
    ARG_UNUSED(context);

    struct bms_persistent_data data;
    int ret = capture_persistent_data(&data);

    if (ret == 0) {
        ret = bms_config_store_save(&config_store, &data);
    }

    return ret;
}

static const struct bms_comm_config_ops comm_config_ops = {
    .get_applied = comm_get_applied_config,
    .stage = comm_stage_config,
    .apply = comm_apply_config,
    .save = comm_save_config,
};

static int send_virtual_rs485_request(
    uint8_t type,
    const uint8_t *payload,
    size_t payload_len)
{
    if (payload_len > BMS_RS485_MAX_PAYLOAD) {
        return -E2BIG;
    }

    struct bms_rs485_frame request = {
        .type = type,
        .sequence = virtual_request_sequence++,
        .payload_len = (uint16_t)payload_len,
    };

    if (payload_len > 0U && payload != NULL) {
        memcpy(request.payload, payload, payload_len);
    }

    uint8_t bytes[BMS_RS485_MAX_FRAME_SIZE];
    size_t bytes_len = 0U;

    int ret = bms_protocol_rs485_encode(&request,
                                        bytes,
                                        sizeof(bytes),
                                        &bytes_len);
    if (ret != 0) {
        return ret;
    }

    return bms_communications_handle_rs485_request(
        &communications,
        bytes,
        bytes_len);
}

static void print_config(const struct shell *sh,
                         const char *name,
                         const struct bms_app_config *c)
{
    shell_print(sh, "%s config v%u", name, c->schema_version);
    shell_print(sh, "  periods: measurement=%ums runtime=%ums",
                c->measurement_period_ms,
                c->runtime_period_ms);

    shell_print(sh,
                "  UV: trip=%" PRId32 "mV release=%" PRId32 "mV",
                c->protection.cell_uv_trip_mv,
                c->protection.cell_uv_release_mv);
    shell_print(sh,
                "  OV: trip=%" PRId32 "mV release=%" PRId32 "mV",
                c->protection.cell_ov_trip_mv,
                c->protection.cell_ov_release_mv);

    shell_print(sh,
                "  charge OC: trip=%" PRId32 "mA release=%" PRId32 "mA",
                c->protection.charge_oc_trip_ma,
                c->protection.charge_oc_release_ma);
    shell_print(sh,
                "  discharge OC: trip=%" PRId32 "mA release=%" PRId32 "mA",
                c->protection.discharge_oc_trip_ma,
                c->protection.discharge_oc_release_ma);

    shell_print(sh,
                "  stale timeout=%ums",
                c->protection.max_snapshot_age_ms);

    shell_print(sh,
                "  balancing: enabled=%u start=%" PRId32
                "mV start_delta=%" PRId32 "mV stop_delta=%" PRId32
                "mV max_current=%" PRId32 "mA max_cells=%u",
                c->balancing.enabled,
                c->balancing.start_cell_mv,
                c->balancing.start_delta_mv,
                c->balancing.stop_delta_mv,
                c->balancing.max_abs_current_ma,
                c->balancing.max_simultaneous_cells);

    shell_print(sh,
                "  estimator: design=%" PRIu32
                "mAh initial_full=%" PRIu32
                "mAh initial_SOC=%u.%02u%% efficiency=%u.%u%%",
                c->estimator.design_capacity_mAh,
                c->estimator.initial_full_capacity_mAh,
                c->estimator.initial_soc_centi_pct / 100U,
                c->estimator.initial_soc_centi_pct % 100U,
                c->estimator.charge_efficiency_permille / 10U,
                c->estimator.charge_efficiency_permille % 10U);
}

static int cmd_config_show(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    print_config(sh, "APPLIED", &applied_config);

    if (!bms_config_equal(&staged_config, &applied_config)) {
        shell_print(sh, "");
        print_config(sh, "STAGED", &staged_config);
        shell_warn(sh, "staged config differs; run 'bms config apply' to use it");
    }

    return 0;
}

static int cmd_config_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_persistent_data saved;
    int ret = bms_config_store_load(&config_store, &saved);

    shell_print(sh, "dirty=%u generation=%" PRIu32,
                !bms_config_equal(&staged_config, &applied_config),
                bms_config_store_last_generation(&config_store));

    if (ret == 0) {
        shell_print(sh, "saved_image=VALID estimator_state=%u",
                    saved.estimator_state_valid);
        return 0;
    }

    if (ret == -ENOENT) {
        shell_print(sh, "saved_image=EMPTY");
        return 0;
    }

    shell_error(sh, "saved_image=INVALID load_error=%d", ret);
    return ret;
}

static int cmd_config_defaults(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    bms_config_defaults(&staged_config);
    shell_print(sh, "factory defaults loaded into STAGED config; not applied yet");
    return 0;
}

static int cmd_config_apply(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = apply_staged_config(true);

    if (ret == 0) {
        shell_print(sh, "staged config validated and applied");
    } else {
        shell_error(sh, "config apply failed: %d", ret);
    }

    return ret;
}

static int cmd_config_save(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_persistent_data data;
    int ret = capture_persistent_data(&data);

    if (ret == 0) {
        ret = bms_config_store_save(&config_store, &data);
    }

    if (ret == 0) {
        shell_print(sh, "APPLIED config + estimator state saved; generation=%" PRIu32,
                    bms_config_store_last_generation(&config_store));
    }

    return ret;
}

static int cmd_config_load(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_persistent_data data;
    int ret = bms_config_store_load(&config_store, &data);

    if (ret == 0) {
        staged_config = data.config;
        shell_print(sh,
                    "saved config loaded into STAGED config; run 'bms config apply'");
        if (data.estimator_state_valid) {
            shell_print(sh,
                        "note: estimator retained state is restored only by 'simulate_reboot'");
        }
    }

    return ret;
}

static int cmd_config_erase(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_config_store_erase(&config_store);

    if (ret == 0) {
        shell_print(sh, "persistent image erased");
    }

    return ret;
}

static int cmd_config_simulate_reboot(const struct shell *sh,
                                      size_t argc,
                                      char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_persistent_data data;
    int ret = bms_config_store_load(&config_store, &data);

    if (ret != 0) {
        shell_error(sh, "no valid saved image to restore: %d", ret);
        return ret;
    }

    ret = restore_persistent_data(&data);

    if (ret == 0) {
        shell_print(sh,
                    "simulated reboot restore complete: config + retained estimator state");
    }

    return ret;
}

static int cmd_config_set_periods(const struct shell *sh,
                                  size_t argc,
                                  char **argv)
{
    if (argc != 3U) return -EINVAL;

    unsigned long measurement_ms = strtoul(argv[1], NULL, 0);
    unsigned long runtime_ms = strtoul(argv[2], NULL, 0);

    if (measurement_ms == 0U || measurement_ms > UINT32_MAX ||
        runtime_ms == 0U || runtime_ms > UINT32_MAX) {
        return -EINVAL;
    }

    staged_config.measurement_period_ms = (uint32_t)measurement_ms;
    staged_config.runtime_period_ms = (uint32_t)runtime_ms;

    shell_print(sh,
                "STAGED periods -> measurement=%lums runtime=%lums",
                measurement_ms, runtime_ms);
    return 0;
}

static int cmd_config_set_uv(const struct shell *sh,
                             size_t argc,
                             char **argv)
{
    if (argc != 3U) return -EINVAL;

    staged_config.protection.cell_uv_trip_mv =
        (int32_t)strtol(argv[1], NULL, 0);
    staged_config.protection.cell_uv_release_mv =
        (int32_t)strtol(argv[2], NULL, 0);

    shell_print(sh, "STAGED UV updated");
    return 0;
}

static int cmd_config_set_ov(const struct shell *sh,
                             size_t argc,
                             char **argv)
{
    if (argc != 3U) return -EINVAL;

    staged_config.protection.cell_ov_trip_mv =
        (int32_t)strtol(argv[1], NULL, 0);
    staged_config.protection.cell_ov_release_mv =
        (int32_t)strtol(argv[2], NULL, 0);

    shell_print(sh, "STAGED OV updated");
    return 0;
}

static int cmd_config_set_charge_oc(const struct shell *sh,
                                    size_t argc,
                                    char **argv)
{
    if (argc != 3U) return -EINVAL;

    staged_config.protection.charge_oc_trip_ma =
        (int32_t)strtol(argv[1], NULL, 0);
    staged_config.protection.charge_oc_release_ma =
        (int32_t)strtol(argv[2], NULL, 0);

    shell_print(sh, "STAGED charge OC updated");
    return 0;
}

static int cmd_config_set_discharge_oc(const struct shell *sh,
                                       size_t argc,
                                       char **argv)
{
    if (argc != 3U) return -EINVAL;

    staged_config.protection.discharge_oc_trip_ma =
        (int32_t)strtol(argv[1], NULL, 0);
    staged_config.protection.discharge_oc_release_ma =
        (int32_t)strtol(argv[2], NULL, 0);

    shell_print(sh, "STAGED discharge OC updated");
    return 0;
}

static int cmd_config_set_capacity(const struct shell *sh,
                                   size_t argc,
                                   char **argv)
{
    if (argc != 3U) return -EINVAL;

    unsigned long design = strtoul(argv[1], NULL, 0);
    unsigned long initial_full = strtoul(argv[2], NULL, 0);

    if (design == 0U || design > UINT32_MAX ||
        initial_full == 0U || initial_full > UINT32_MAX) {
        return -EINVAL;
    }

    staged_config.estimator.design_capacity_mAh = (uint32_t)design;
    staged_config.estimator.initial_full_capacity_mAh =
        (uint32_t)initial_full;

    shell_print(sh,
                "STAGED capacity -> design=%lumAh initial_full=%lumAh",
                design, initial_full);
    return 0;
}

/* ----- Measurement shell ----- */

static int cmd_measurement_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_print(sh,
                "running=%u valid=%u period=%ums age=%" PRId64 "ms last_error=%d",
                atomic_get(&measurement.running) != 0,
                bms_measurement_has_snapshot(&measurement),
                bms_measurement_period_ms(&measurement),
                bms_measurement_snapshot_age_ms(&measurement),
                bms_measurement_last_error(&measurement));
    return 0;
}

static int cmd_measurement_refresh(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_measurement_refresh_now(&measurement);
    if (!ret) shell_print(sh, "snapshot refreshed and published");
    return ret;
}

static int cmd_measurement_start(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_measurement_start(&measurement);
    if (!ret) shell_print(sh, "periodic measurement started");
    return ret;
}

static int cmd_measurement_stop(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_measurement_stop(&measurement);
    if (!ret) shell_print(sh, "periodic measurement stopped; last snapshot remains readable");
    return ret;
}

static int cmd_measurement_period(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2U) return -EINVAL;

    unsigned long ms = strtoul(argv[1], NULL, 0);
    if (ms == 0U || ms > UINT32_MAX) return -EINVAL;

    int ret = bms_measurement_set_period(&measurement, (uint32_t)ms);
    if (!ret) shell_print(sh, "measurement period -> %lums", ms);
    return ret;
}

/* ----- Core runtime / protection / state shell ----- */

static void print_reasons(const struct shell *sh, uint64_t reasons)
{
    if (reasons == 0U) {
        shell_print(sh, "  reasons: none");
        return;
    }

    shell_print(sh, "  reasons:");
    for (uint8_t bit = 0U; bit < 64U; ++bit) {
        uint64_t mask = (1ULL << bit);
        if ((reasons & mask) != 0U) {
            shell_print(sh, "    - %s", bms_protection_reason_name(mask));
        }
    }
}

static int cmd_protection_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_protection_decision d;
    int ret = bms_core_runtime_get_decision(&runtime, &d);
    if (ret) {
        shell_error(sh, "no protection decision yet: %d", ret);
        return ret;
    }

    shell_print(sh, "snapshot=%" PRIu32 " CHG_allowed=%u DSG_allowed=%u reasons=0x%016" PRIx64,
                d.snapshot_sequence,
                d.charge_allowed,
                d.discharge_allowed,
                d.reasons);
    print_reasons(sh, d.reasons);
    return 0;
}

static int cmd_protection_config(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_warn(sh, "simulation/development limits only; not approved product thresholds");
    shell_print(sh, "cell UV: trip=%" PRId32 "mV release=%" PRId32 "mV",
                applied_config.protection.cell_uv_trip_mv,
                applied_config.protection.cell_uv_release_mv);
    shell_print(sh, "cell OV: trip=%" PRId32 "mV release=%" PRId32 "mV",
                applied_config.protection.cell_ov_trip_mv,
                applied_config.protection.cell_ov_release_mv);
    shell_print(sh, "charge OC: trip=%" PRId32 "mA release=%" PRId32 "mA",
                applied_config.protection.charge_oc_trip_ma,
                applied_config.protection.charge_oc_release_ma);
    shell_print(sh, "discharge OC: trip=%" PRId32 "mA release=%" PRId32 "mA",
                applied_config.protection.discharge_oc_trip_ma,
                applied_config.protection.discharge_oc_release_ma);
    shell_print(sh, "max snapshot age=%ums",
                applied_config.protection.max_snapshot_age_ms);
    return 0;
}

static int cmd_state(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_print(sh, "state=%s actuation=%u last_actuation_error=%d",
                bms_state_name(runtime.state_machine.state),
                runtime.state_machine.actuation_enabled,
                runtime.state_machine.last_actuation_error);
    return 0;
}

static int cmd_runtime_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_print(sh,
                "running=%u period=%ums last_seq=%" PRIu32 " last_error=%d",
                atomic_get(&runtime.running) != 0,
                bms_core_runtime_period_ms(&runtime),
                runtime.last_processed_sequence,
                bms_core_runtime_last_error(&runtime));

    shell_print(sh, "state=%s",
                bms_state_name(runtime.state_machine.state));
    return 0;
}

static int cmd_runtime_process(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_core_runtime_process_now(&runtime);
    if (!ret) shell_print(sh, "core runtime processed latest snapshot");
    return ret;
}

static int cmd_runtime_start(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_core_runtime_start(&runtime);
    if (!ret) shell_print(sh, "core runtime started");
    return ret;
}

static int cmd_runtime_stop(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_core_runtime_stop(&runtime);
    if (!ret) shell_print(sh, "core runtime stopped; last decision/state remains readable");
    return ret;
}

static void print_balance_inhibits(const struct shell *sh, uint32_t reasons)
{
    if (reasons == 0U) {
        shell_print(sh, "  inhibits: none");
        return;
    }

    shell_print(sh, "  inhibits:");
    for (uint8_t bit = 0U; bit < 32U; ++bit) {
        uint32_t mask = (1U << bit);
        if ((reasons & mask) != 0U) {
            shell_print(sh, "    - %s", bms_balancing_inhibit_name(mask));
        }
    }
}

static int cmd_balancing_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_balancing_decision d;
    int ret = bms_core_runtime_get_balancing_decision(&runtime, &d);
    if (ret) {
        shell_error(sh, "no balancing decision yet: %d", ret);
        return ret;
    }

    shell_print(sh,
                "snapshot=%" PRIu32 " enabled=%u desired=0x%08" PRIx64
                " applied=0x%08" PRIx64 " eligible=0x%08" PRIx64,
                d.snapshot_sequence,
                runtime.balancing.config.enabled,
                d.desired_mask,
                runtime.applied_balance_mask,
                d.eligible_mask);
    print_balance_inhibits(sh, d.inhibit_reasons);
    return 0;
}

static int cmd_balancing_config(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    const struct bms_balancing_config *c = &runtime.balancing.config;
    shell_warn(sh, "simulation/development values only; not approved product limits");
    shell_print(sh, "enabled=%u start_cell=%" PRId32 "mV start_delta=%" PRId32
                    "mV stop_delta=%" PRId32 "mV",
                c->enabled, c->start_cell_mv, c->start_delta_mv, c->stop_delta_mv);
    shell_print(sh, "max_abs_current=%" PRId32 "mA max_simultaneous_cells=%u",
                c->max_abs_current_ma, c->max_simultaneous_cells);
    return 0;
}

static int cmd_balancing_enable(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int ret = bms_balancing_set_enabled(&runtime.balancing, true);
    if (!ret) shell_print(sh, "automatic balancing policy enabled");
    return ret;
}

static int cmd_balancing_disable(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    int ret = bms_balancing_set_enabled(&runtime.balancing, false);
    if (ret) return ret;

    /* Clear hardware request immediately, don't wait for next runtime tick. */
    ret = bms_topology_set_balance_mask(&topo, 0U);
    if (!ret) {
        runtime.applied_balance_mask = 0U;
        shell_print(sh, "automatic balancing disabled; applied mask cleared");
    }
    return ret;
}

/* ----- SOC / charge / energy estimator shell ----- */

static int cmd_estimator_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_estimator_status s;
    int ret = bms_core_runtime_get_estimator_status(&runtime, &s);
    if (ret) {
        shell_error(sh, "no estimator status yet: %d", ret);
        return ret;
    }

    shell_print(sh,
                "SOC=%u.%02u%% SOH(capacity-model)=%u.%02u%% snapshot=%" PRIu32,
                s.soc_centi_pct / 100U,
                s.soc_centi_pct % 100U,
                s.soh_centi_pct / 100U,
                s.soh_centi_pct % 100U,
                s.last_snapshot_sequence);

    shell_print(sh,
                "capacity: design=%" PRIu32 "mAh estimated_full=%" PRIu32
                "mAh remaining=%" PRId64 "uAh",
                s.design_capacity_mAh,
                s.estimated_full_capacity_mAh,
                s.remaining_charge_uAh);

    shell_print(sh,
                "charge counters: in=%" PRIu64 "uAh out=%" PRIu64
                "uAh throughput=%" PRIu64 "uAh",
                s.charged_uAh,
                s.discharged_uAh,
                s.throughput_uAh);

    shell_print(sh,
                "energy: in=%" PRIu64 "uWh out=%" PRIu64
                "uWh EFC=%" PRIu32 ".%03" PRIu32
                " skipped_intervals=%" PRIu32,
                s.energy_in_uWh,
                s.energy_out_uWh,
                s.equivalent_full_cycles_milli / 1000U,
                s.equivalent_full_cycles_milli % 1000U,
                s.skipped_intervals);

    return 0;
}

static int cmd_estimator_config(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_warn(sh,
               "SOC is coulomb-counted; automatic SOH capacity learning is NOT implemented yet");

    shell_print(sh,
                "design_capacity=%" PRIu32 "mAh initial_full=%" PRIu32
                "mAh initial_SOC=%u.%02u%%",
                applied_config.estimator.design_capacity_mAh,
                applied_config.estimator.initial_full_capacity_mAh,
                applied_config.estimator.initial_soc_centi_pct / 100U,
                applied_config.estimator.initial_soc_centi_pct % 100U);

    shell_print(sh,
                "charge_efficiency=%u.%u%% max_integration_gap=%" PRIu32 "ms",
                applied_config.estimator.charge_efficiency_permille / 10U,
                applied_config.estimator.charge_efficiency_permille % 10U,
                applied_config.estimator.max_integration_interval_ms);

    return 0;
}

static int cmd_estimator_set_soc(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2U) return -EINVAL;

    unsigned long percent = strtoul(argv[1], NULL, 0);
    if (percent > 100U) return -EINVAL;

    int ret = bms_core_runtime_set_soc_centi_pct(
        &runtime, (uint16_t)(percent * 100U));

    if (!ret) {
        shell_print(sh, "SOC calibration -> %lu%%", percent);
    }

    return ret;
}

static int cmd_estimator_set_full_capacity(const struct shell *sh,
                                           size_t argc,
                                           char **argv)
{
    if (argc != 2U) return -EINVAL;

    unsigned long mAh = strtoul(argv[1], NULL, 0);
    if (mAh == 0U || mAh > UINT32_MAX) return -EINVAL;

    int ret = bms_core_runtime_set_full_capacity_mAh(
        &runtime, (uint32_t)mAh);

    if (!ret) {
        shell_print(sh,
                    "estimated usable full capacity -> %lumAh; SOH ratio updated",
                    mAh);
    }

    return ret;
}

static int cmd_estimator_reset_counters(const struct shell *sh,
                                        size_t argc,
                                        char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_core_runtime_reset_estimator_counters(&runtime);
    if (!ret) {
        shell_print(sh,
                    "charge/energy/throughput counters reset; SOC unchanged");
    }
    return ret;
}

static int cmd_events_show(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_event_record records[CONFIG_BMS_EVENT_LOG_CAPACITY];
    size_t count = 0U;

    int ret = bms_events_copy(&runtime.events,
                              records,
                              CONFIG_BMS_EVENT_LOG_CAPACITY,
                              &count);
    if (ret) return ret;

    shell_print(sh, "events=%u", (unsigned)count);

    for (size_t i = 0U; i < count; ++i) {
        shell_print(sh,
                    "#%" PRIu32 " t=%" PRId64 "ms snap=%" PRIu32
                    " %-20s data=0x%016" PRIx64 " detail=%" PRId32,
                    records[i].id,
                    records[i].timestamp_ms,
                    records[i].snapshot_sequence,
                    bms_event_type_name(records[i].type),
                    records[i].data,
                    records[i].detail);
    }

    return 0;
}

static int cmd_events_clear(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_events_clear(&runtime.events);
    if (!ret) shell_print(sh, "event log cleared");
    return ret;
}


/* ----- System supervisor / diagnostics shell ----- */

static void print_health_flags(const struct shell *sh, uint64_t flags)
{
    if (flags == 0U) {
        shell_print(sh, "  health flags: none");
        return;
    }

    shell_print(sh, "  health flags:");

    for (uint8_t bit = 0U; bit < 64U; ++bit) {
        uint64_t mask = 1ULL << bit;

        if ((flags & mask) != 0U) {
            shell_print(sh,
                        "    - %s",
                        bms_system_health_flag_name(mask));
        }
    }
}

static int cmd_system_status(const struct shell *sh,
                             size_t argc,
                             char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_system_health h;
    int ret = bms_system_get_health(&system_supervisor, &h);
    if (ret != 0) return ret;

    shell_print(sh,
                "mode=%s self_test=%s safe_latched=%u maintenance=%u",
                bms_system_mode_name(h.mode),
                h.self_test_passed ? "PASS" : "FAIL",
                h.safe_latched,
                h.maintenance_mode);

    shell_print(sh,
                "snapshot: seq=%" PRIu32 " age=%" PRId64
                "ms runtime_seq=%" PRIu32 " CAN_cycles=%" PRIu32,
                h.snapshot_sequence,
                h.snapshot_age_ms,
                h.runtime_sequence,
                h.can_publish_cycles);

    shell_print(sh,
                "errors: measurement=%d runtime=%d comm=%d watchdog=%d",
                h.measurement_error,
                h.runtime_error,
                h.communication_error,
                h.watchdog_error);

    shell_print(sh,
                "self_test_failed_checks=0x%08" PRIx32,
                h.self_test_failed_checks);

    print_health_flags(sh, h.flags);
    return 0;
}

static int cmd_system_selftest(const struct shell *sh,
                               size_t argc,
                               char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_system_run_self_test(&system_supervisor);

    if (ret == 0) {
        shell_print(sh, "startup/self-test: PASS");
    } else {
        shell_error(sh, "startup/self-test: FAIL (%d)", ret);
    }

    return ret;
}

static int cmd_system_check(const struct shell *sh,
                            size_t argc,
                            char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_system_check_now(&system_supervisor);

    if (ret == 0) {
        return cmd_system_status(sh, 0U, NULL);
    }

    return ret;
}

static int cmd_system_recover(const struct shell *sh,
                              size_t argc,
                              char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_system_recover(&system_supervisor);

    if (ret == 0) {
        shell_print(sh, "explicit system recovery completed");
    } else {
        shell_error(sh, "system recovery failed: %d", ret);
    }

    return ret;
}

static int cmd_system_watchdog(const struct shell *sh,
                               size_t argc,
                               char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_print(sh,
                "virtual watchdog: feeds=%" PRIu32
                " last_feed=%" PRId64 "ms injected_error=%d",
                virtual_watchdog.feed_count,
                virtual_watchdog.last_feed_ms,
                virtual_watchdog.injected_error);

    return 0;
}

static int cmd_system_watchdog_error(const struct shell *sh,
                                     size_t argc,
                                     char **argv)
{
    if (argc != 2U) return -EINVAL;

    int error = (int)strtol(argv[1], NULL, 0);
    bms_virtual_watchdog_set_error(&virtual_watchdog, error);

    shell_print(sh, "virtual watchdog feed error -> %d", error);
    return 0;
}

/* ----- Communications shell ----- */

static int cmd_comm_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    struct bms_communications_stats s;
    int ret = bms_communications_get_stats(&communications, &s);
    if (ret != 0) return ret;

    shell_print(sh,
                "running=%u CAN_period=%ums CAN_cycles=%" PRIu32
                " CAN_frames=%" PRIu32,
                atomic_get(&communications.running) != 0,
                communications.can_period_ms,
                s.can_publish_cycles,
                s.can_frames_sent);

    shell_print(sh,
                "RS485_requests=%" PRIu32 " responses=%" PRIu32
                " errors=%" PRIu32 " last_error=%d last_snapshot=%" PRIu32,
                s.rs485_requests,
                s.rs485_responses,
                s.errors,
                s.last_error,
                s.last_snapshot_sequence);

    shell_print(sh,
                "remote_config_write=%u",
                communications.allow_remote_config_write);

    return 0;
}

static int cmd_comm_can_publish(const struct shell *sh,
                                size_t argc,
                                char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = bms_communications_publish_can_now(&communications);
    if (!ret) {
        shell_print(sh,
                    "CAN telemetry published; total virtual frames=%" PRIu32,
                    virtual_can.total_sent);
    }

    return ret;
}

static int cmd_comm_can_dump(const struct shell *sh,
                             size_t argc,
                             char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    shell_print(sh,
                "virtual CAN history=%u total_sent=%" PRIu32,
                (unsigned)virtual_can.count,
                virtual_can.total_sent);

    for (size_t i = 0U; i < virtual_can.count; ++i) {
        const struct bms_can_frame *f = &virtual_can.frames[i];

        shell_fprintf(sh, SHELL_NORMAL,
                      "ID=0x%03" PRIx32 " DLC=%u DATA=",
                      f->id,
                      f->dlc);

        for (uint8_t b = 0U; b < f->dlc; ++b) {
            shell_fprintf(sh, SHELL_NORMAL,
                          "%02x%s",
                          f->data[b],
                          b + 1U == f->dlc ? "" : " ");
        }

        shell_fprintf(sh, SHELL_NORMAL, "\n");
    }

    return 0;
}

static int cmd_comm_rs485_request(const struct shell *sh,
                                  size_t argc,
                                  char **argv)
{
    if (argc != 2U) return -EINVAL;

    uint8_t type;

    if (strcmp(argv[1], "status") == 0) {
        type = BMS_RS485_REQ_STATUS;
    } else if (strcmp(argv[1], "cells") == 0) {
        type = BMS_RS485_REQ_CELLS;
    } else if (strcmp(argv[1], "config") == 0) {
        type = BMS_RS485_REQ_CONFIG;
    } else {
        return -EINVAL;
    }

    int ret = send_virtual_rs485_request(type, NULL, 0U);

    if (!ret) {
        shell_print(sh,
                    "RS485 request %s handled",
                    bms_rs485_message_type_name(type));
    }

    return ret;
}

static int cmd_comm_rs485_stage_uv(const struct shell *sh,
                                   size_t argc,
                                   char **argv)
{
    if (argc != 3U) return -EINVAL;

    struct bms_app_config remote_cfg = applied_config;

    remote_cfg.protection.cell_uv_trip_mv =
        (int32_t)strtol(argv[1], NULL, 0);
    remote_cfg.protection.cell_uv_release_mv =
        (int32_t)strtol(argv[2], NULL, 0);

    uint8_t payload[BMS_RS485_MAX_PAYLOAD];
    size_t payload_len = 0U;

    int ret = bms_protocol_encode_config_payload(
        &remote_cfg,
        payload,
        sizeof(payload),
        &payload_len);

    if (ret == 0) {
        ret = send_virtual_rs485_request(
            BMS_RS485_REQ_SET_STAGED_CONFIG,
            payload,
            payload_len);
    }

    if (ret == 0) {
        shell_print(sh,
                    "remote RS485 staged UV -> %" PRId32 "/%" PRId32 "mV",
                    remote_cfg.protection.cell_uv_trip_mv,
                    remote_cfg.protection.cell_uv_release_mv);
    }

    return ret;
}

static int cmd_comm_rs485_apply(const struct shell *sh,
                                size_t argc,
                                char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = send_virtual_rs485_request(
        BMS_RS485_REQ_APPLY_CONFIG,
        NULL,
        0U);

    if (!ret) {
        shell_print(sh, "remote RS485 config apply request completed");
    }

    return ret;
}

static int cmd_comm_rs485_save(const struct shell *sh,
                               size_t argc,
                               char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    int ret = send_virtual_rs485_request(
        BMS_RS485_REQ_SAVE_CONFIG,
        NULL,
        0U);

    if (!ret) {
        shell_print(sh, "remote RS485 config save request completed");
    }

    return ret;
}

static int cmd_comm_rs485_dump(const struct shell *sh,
                               size_t argc,
                               char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    if (virtual_rs485.last_len == 0U) {
        shell_print(sh, "no RS485 response captured yet");
        return 0;
    }

    struct bms_rs485_frame frame;
    int ret = bms_protocol_rs485_decode(
        virtual_rs485.last_frame,
        virtual_rs485.last_len,
        &frame);

    if (ret != 0) {
        shell_error(sh, "last RS485 frame decode failed: %d", ret);
        return ret;
    }

    shell_print(sh,
                "last response: %s seq=%u payload=%u bytes total_wire=%u",
                bms_rs485_message_type_name(frame.type),
                frame.sequence,
                frame.payload_len,
                (unsigned)virtual_rs485.last_len);

    if (frame.type == BMS_RS485_RSP_STATUS) {
        struct bms_telemetry_view view;
        ret = bms_protocol_decode_status_payload(
            frame.payload,
            frame.payload_len,
            &view);

        if (ret == 0) {
            shell_print(sh,
                        "  pack=%" PRId32 "mV current=%" PRId32
                        "mA SOC=%u.%02u%% state=%s faults=0x%016" PRIx64,
                        view.pack_mv,
                        view.current_ma,
                        view.soc_centi_pct / 100U,
                        view.soc_centi_pct % 100U,
                        bms_state_name((enum bms_operating_state)view.state),
                        view.faults);
        }
    } else if (frame.type == BMS_RS485_RSP_CELLS) {
        struct bms_telemetry_view view = {0};

        ret = bms_protocol_decode_cells_payload(
            frame.payload,
            frame.payload_len,
            &view);

        if (ret == 0) {
            shell_print(sh, "  cell_count=%u", view.cell_count);

            for (uint8_t i = 0U; i < view.cell_count; ++i) {
                shell_print(sh,
                            "  cell[%02u]=%" PRId32 "mV",
                            i + 1U,
                            view.cell_mv[i]);
            }
        }
    } else if (frame.type == BMS_RS485_RSP_CONFIG) {
        struct bms_app_config config;

        ret = bms_protocol_decode_config_payload(
            frame.payload,
            frame.payload_len,
            &config);

        if (ret == 0) {
            shell_print(sh,
                        "  config v%u measurement=%ums runtime=%ums UV=%" PRId32
                        "/%" PRId32 "mV",
                        config.schema_version,
                        config.measurement_period_ms,
                        config.runtime_period_ms,
                        config.protection.cell_uv_trip_mv,
                        config.protection.cell_uv_release_mv);
        }
    } else if (frame.type == BMS_RS485_RSP_ACK ||
               frame.type == BMS_RS485_RSP_ERROR) {
        if (frame.payload_len == 4U) {
            int32_t result =
                (int32_t)((uint32_t)frame.payload[0] |
                          ((uint32_t)frame.payload[1] << 8) |
                          ((uint32_t)frame.payload[2] << 16) |
                          ((uint32_t)frame.payload[3] << 24));

            shell_print(sh, "  result=%" PRId32, result);
        }
    }

    return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_system_watchdog,
    SHELL_CMD(status, NULL, "Show virtual watchdog state",
              cmd_system_watchdog),
    SHELL_CMD_ARG(inject_error, NULL, "<errno; 0 clears>",
                  cmd_system_watchdog_error, 2, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_system,
    SHELL_CMD(status, NULL, "Show startup/health/safe-mode status",
              cmd_system_status),
    SHELL_CMD(selftest, NULL, "Run startup self-test",
              cmd_system_selftest),
    SHELL_CMD(check, NULL, "Run health supervisor immediately",
              cmd_system_check),
    SHELL_CMD(recover, NULL, "Explicitly recover from SAFE_LATCHED",
              cmd_system_recover),
    SHELL_CMD(watchdog, &sub_system_watchdog, "Watchdog policy/backend",
              NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_scenario,
    SHELL_CMD(all, NULL, "Run deterministic BMS regression scenarios",
              cmd_scenario_all),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_comm_can,
    SHELL_CMD(publish, NULL, "Publish one CAN telemetry cycle",
              cmd_comm_can_publish),
    SHELL_CMD(dump, NULL, "Dump virtual CAN history",
              cmd_comm_can_dump),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_comm_rs485,
    SHELL_CMD_ARG(request, NULL, "status|cells|config",
                  cmd_comm_rs485_request, 2, 0),
    SHELL_CMD_ARG(stage_uv, NULL, "<trip_mV> <release_mV>",
                  cmd_comm_rs485_stage_uv, 3, 0),
    SHELL_CMD(apply, NULL, "Apply remotely staged config",
              cmd_comm_rs485_apply),
    SHELL_CMD(save, NULL, "Persist applied config remotely",
              cmd_comm_rs485_save),
    SHELL_CMD(dump, NULL, "Decode last virtual RS485 response",
              cmd_comm_rs485_dump),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_comm,
    SHELL_CMD(status, NULL, "Show communication service stats",
              cmd_comm_status),
    SHELL_CMD(can, &sub_comm_can, "CAN reference telemetry",
              NULL),
    SHELL_CMD(rs485, &sub_comm_rs485, "RS485 framed protocol",
              NULL),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config_set,
    SHELL_CMD_ARG(periods, NULL, "<measurement_ms> <runtime_ms>",
                  cmd_config_set_periods, 3, 0),
    SHELL_CMD_ARG(uv, NULL, "<trip_mV> <release_mV>",
                  cmd_config_set_uv, 3, 0),
    SHELL_CMD_ARG(ov, NULL, "<trip_mV> <release_mV>",
                  cmd_config_set_ov, 3, 0),
    SHELL_CMD_ARG(charge_oc, NULL, "<trip_mA> <release_mA>",
                  cmd_config_set_charge_oc, 3, 0),
    SHELL_CMD_ARG(discharge_oc, NULL, "<trip_mA> <release_mA>",
                  cmd_config_set_discharge_oc, 3, 0),
    SHELL_CMD_ARG(capacity, NULL, "<design_mAh> <initial_full_mAh>",
                  cmd_config_set_capacity, 3, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config,
    SHELL_CMD(show, NULL, "Show applied/staged configuration", cmd_config_show),
    SHELL_CMD(status, NULL, "Show dirty/persistent status", cmd_config_status),
    SHELL_CMD(defaults, NULL, "Load factory defaults into STAGED config",
              cmd_config_defaults),
    SHELL_CMD(set, &sub_config_set, "Edit STAGED configuration", NULL),
    SHELL_CMD(apply, NULL, "Validate and apply STAGED config", cmd_config_apply),
    SHELL_CMD(save, NULL, "Persist APPLIED config + estimator state",
              cmd_config_save),
    SHELL_CMD(load, NULL, "Load saved config into STAGED config",
              cmd_config_load),
    SHELL_CMD(simulate_reboot, NULL,
              "Restore saved config + estimator state like boot",
              cmd_config_simulate_reboot),
    SHELL_CMD(erase, NULL, "Erase persistent image", cmd_config_erase),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_measurement,
    SHELL_CMD(status, NULL, "Show measurement/cache status", cmd_measurement_status),
    SHELL_CMD(refresh, NULL, "Refresh and publish immediately", cmd_measurement_refresh),
    SHELL_CMD(start, NULL, "Start periodic measurement", cmd_measurement_start),
    SHELL_CMD(stop, NULL, "Stop periodic measurement", cmd_measurement_stop),
    SHELL_CMD_ARG(period, NULL, "Set period in ms", cmd_measurement_period, 2, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_runtime,
    SHELL_CMD(status, NULL, "Show core-runtime status", cmd_runtime_status),
    SHELL_CMD(process, NULL, "Process latest snapshot immediately", cmd_runtime_process),
    SHELL_CMD(start, NULL, "Start periodic core runtime", cmd_runtime_start),
    SHELL_CMD(stop, NULL, "Stop periodic core runtime", cmd_runtime_stop),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_protection,
    SHELL_CMD(status, NULL, "Show latest protection decision", cmd_protection_status),
    SHELL_CMD(config, NULL, "Show development protection thresholds", cmd_protection_config),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_balancing,
    SHELL_CMD(status, NULL, "Show balancing policy decision", cmd_balancing_status),
    SHELL_CMD(config, NULL, "Show development balancing policy", cmd_balancing_config),
    SHELL_CMD(enable, NULL, "Enable automatic balancing policy", cmd_balancing_enable),
    SHELL_CMD(disable, NULL, "Disable policy and clear balance mask", cmd_balancing_disable),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_estimator,
    SHELL_CMD(status, NULL, "Show SOC/SOH and charge/energy counters", cmd_estimator_status),
    SHELL_CMD(config, NULL, "Show estimator configuration/caveats", cmd_estimator_config),
    SHELL_CMD_ARG(set_soc, NULL, "Calibrate SOC: <0..100 percent>",
                  cmd_estimator_set_soc, 2, 0),
    SHELL_CMD_ARG(set_full_capacity, NULL, "Set estimated full capacity: <mAh>",
                  cmd_estimator_set_full_capacity, 2, 0),
    SHELL_CMD(reset_counters, NULL, "Reset throughput/energy counters only",
              cmd_estimator_reset_counters),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_events,
    SHELL_CMD(show, NULL, "Show in-RAM event log", cmd_events_show),
    SHELL_CMD(clear, NULL, "Clear event log", cmd_events_clear),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_test,
    SHELL_CMD(transport, NULL, "Test virtual transport contract", cmd_test_transport),
    SHELL_CMD(driver, NULL, "Test semantic BQ driver", cmd_test_driver),
    SHELL_CMD(monitor, NULL, "Test BQ monitor adapter", cmd_test_monitor),
    SHELL_CMD(topology, NULL, "Test topology + direct snapshot build", cmd_test_topology),
    SHELL_CMD(measurement, NULL, "Test measurement + published snapshot", cmd_test_measurement),
    SHELL_CMD(runtime, NULL, "Test protection + state machine on current snapshot", cmd_test_runtime),
    SHELL_CMD(balancing, NULL, "Test balancing policy on current snapshot", cmd_test_balancing),
    SHELL_CMD(estimator, NULL, "Test SOC/coulomb estimator on current snapshot", cmd_test_estimator),
    SHELL_CMD(persistence, NULL, "Test config store round-trip + CRC", cmd_test_persistence),
    SHELL_CMD(communications, NULL, "Test CAN + RS485 protocol/service", cmd_test_communications),
    SHELL_CMD(system, NULL, "Test startup/health/watchdog supervisor", cmd_test_system),
    SHELL_CMD(all, NULL, "Test complete virtual stack", cmd_test_all),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_inject,
    SHELL_CMD_ARG(cell, NULL, "<bq 0|1> <cell 1..16> <mV>", cmd_inject_cell, 4, 0),
    SHELL_CMD_ARG(current, NULL, "<bq 0|1> <mA>", cmd_inject_current, 3, 0),
    SHELL_CMD_ARG(uv, NULL, "<bq 0|1> on|off", cmd_inject_uv, 3, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bms,
    SHELL_CMD(test, &sub_test, "Layer-by-layer tests", NULL),
    SHELL_CMD(snapshot, NULL, "Read latest PUBLISHED snapshot", cmd_snapshot),
    SHELL_CMD(config, &sub_config, "Configuration + persistence", NULL),
    SHELL_CMD(comm, &sub_comm, "CAN / RS485 communications", NULL),
    SHELL_CMD(system, &sub_system, "Startup / health / watchdog supervisor", NULL),
    SHELL_CMD(scenario, &sub_scenario, "Full virtual regression scenarios", NULL),
    SHELL_CMD(measurement, &sub_measurement, "Measurement service", NULL),
    SHELL_CMD(runtime, &sub_runtime, "Core decision runtime", NULL),
    SHELL_CMD(protection, &sub_protection, "Protection policy", NULL),
    SHELL_CMD(balancing, &sub_balancing, "Automatic balancing policy", NULL),
    SHELL_CMD(estimator, &sub_estimator, "SOC/SOH + charge/energy estimation", NULL),
    SHELL_CMD(state, NULL, "Show authoritative BMS operating state", cmd_state),
    SHELL_CMD(events, &sub_events, "Runtime event log", NULL),
    SHELL_CMD_ARG(raw, NULL, "Read semantic BQ driver <0|1>", cmd_raw, 2, 0),
    SHELL_CMD(inject, &sub_inject, "Change virtual BQ state", NULL),
    SHELL_CMD_ARG(balance, NULL, "Set global balance mask (diagnostic)", cmd_balance, 2, 0),
    SHELL_CMD_ARG(switch, NULL, "Manual switch command (diagnostic)",
                  cmd_switch, 2, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bms, &sub_bms, "Full BMS virtual runtime", NULL);

int main(void)
{
    virtual_bq_reset();

    bms_config_defaults(&staged_config);
    applied_config = staged_config;

    bms_config_store_virtual_init(&virtual_nv);

    int ret = bms_config_store_init(&config_store,
                                    &bms_config_store_virtual_ops,
                                    &virtual_nv);
    if (ret) { printk("config store init failed: %d\n", ret); return 0; }

    bms_virtual_can_init(&virtual_can);
    bms_virtual_rs485_init(&virtual_rs485);

    ret = bms_monitor_bq76952_init(&bq0, "bq0", 0U);
    if (ret) { printk("bq0 init failed: %d\n", ret); return 0; }

    ret = bms_monitor_bq76952_init(&bq1, "bq1", 1U);
    if (ret) { printk("bq1 init failed: %d\n", ret); return 0; }

    segs[0] = (struct bms_topology_segment_cfg){ &bq0.monitor, 0U, 16U, 0U };
    segs[1] = (struct bms_topology_segment_cfg){ &bq1.monitor, 0U, 16U, 16U };

    cfg = (struct bms_topology_config){
        .name = "virtual-32S",
        .segments = segs,
        .segment_count = 2U,
        .current_provider = &bq0.monitor,
        .pack_voltage_provider = NULL, /* sum of both stacks, like the STM32 32S profile */
        .switch_provider = &bq1.monitor,
    };

    ret = bms_topology_init(&topo, &cfg);
    if (ret) { printk("topology init failed: %d\n", ret); return 0; }

    ret = bms_measurement_init(&measurement, &topo,
                               applied_config.measurement_period_ms);
    if (ret) { printk("measurement init failed: %d\n", ret); return 0; }

    ret = bms_measurement_start(&measurement);
    if (ret) { printk("measurement start failed: %d\n", ret); return 0; }

    /* Publish one snapshot before the runtime starts so it never polls empty. */
    ret = bms_measurement_refresh_now(&measurement);
    if (ret) { printk("initial measurement failed: %d\n", ret); return 0; }

    /*
     * Core runtime polls the latest published snapshot every 100 ms.
     * It does not read the BQs directly.
     */
    ret = bms_core_runtime_init(&runtime,
                                &measurement,
                                &topo,
                                &applied_config.protection,
                                &applied_config.balancing,
                                &applied_config.estimator,
                                applied_config.runtime_period_ms,
                                true);
    if (ret) { printk("runtime init failed: %d\n", ret); return 0; }

    ret = bms_core_runtime_start(&runtime);
    if (ret) { printk("runtime start failed: %d\n", ret); return 0; }

    ret = bms_communications_init(
        &communications,
        &measurement,
        &runtime,
        &bms_virtual_can_ops,
        &virtual_can,
        &bms_virtual_rs485_ops,
        &virtual_rs485,
        &comm_config_ops,
        NULL,
        true,   /* enabled only for native_sim development */
        500U);
    if (ret) { printk("communications init failed: %d\n", ret); return 0; }

    ret = bms_communications_start(&communications);
    if (ret) { printk("communications start failed: %d\n", ret); return 0; }

    bms_virtual_watchdog_init(&virtual_watchdog);

    const struct bms_system_config system_cfg = {
        .supervisor_period_ms = 250U,
        .max_snapshot_age_ms =
            applied_config.protection.max_snapshot_age_ms,
        .max_measurement_no_progress_ms = 1200U,
        .max_runtime_no_progress_ms = 1200U,
        .max_comm_no_progress_ms = 2500U,
    };

    ret = bms_system_init(
        &system_supervisor,
        &measurement,
        &runtime,
        &communications,
        &topo,
        &config_store,
        &applied_config,
        &bms_virtual_watchdog_ops,
        &virtual_watchdog,
        &system_cfg);
    if (ret) { printk("system supervisor init failed: %d\n", ret); return 0; }

    ret = bms_system_run_self_test(&system_supervisor);
    if (ret) { printk("startup self-test failed: %d\n", ret); }

    if (ret == 0) {
        ret = bms_system_start(&system_supervisor);
        if (ret) { printk("system supervisor start failed: %d\n", ret); return 0; }
    }

    printk("\nFull virtual BMS platform ready.\n");
    printk("full virtual platform: measurement/core/estimation/config/comms/system-health\n");
    printk("Config defaults: measurement=%ums runtime=%ums\n",
           applied_config.measurement_period_ms,
           applied_config.runtime_period_ms);
    printk("Try: bms test all\n");
    printk("     bms config show\n");
    printk("     bms comm status\n");
    printk("     bms comm can dump\n");
    printk("     bms comm rs485 request status\n");
    printk("     bms system status\n");
    printk("     bms scenario all\n");
    printk("     bms protection status\n");
    printk("     bms balancing status\n");
    printk("     bms estimator status\n");
    printk("     bms state\n");
    printk("     bms events show\n\n");

    while (1) k_sleep(K_SECONDS(60));
    return 0;
}
