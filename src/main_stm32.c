#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "bms/bms_config.h"
#include "bms/bms_config_store.h"
#include "bms/bms_communications.h"
#include "bms/bms_core_runtime.h"
#include "bms/bms_measurement.h"
#include "bms/bms_system.h"
#include "bms/bms_topology.h"
#include "bms_monitor_bq76952.h"
#include "bq76952_reg.h"

#include "bms_board_stm32.h"
#include "bms_communications_stm32.h"
#include "bms_watchdog_stm32.h"
#if defined(CONFIG_BMS_STM32_USE_NVS)
#include "bms_config_store_nvs.h"
#endif

static struct bms_monitor_bq76952 bq0;
#if defined(CONFIG_BMS_PACK_32S)
static struct bms_monitor_bq76952 bq1;
#endif

static struct bms_topology topology;
static struct bms_topology_segment_cfg segments[2];
static struct bms_topology_config topology_cfg;
static struct bms_measurement measurement;
static struct bms_core_runtime runtime;
static struct bms_communications communications;
static struct bms_system supervisor;
static struct bms_stm32_comm_backend comm_hw;
static struct bms_stm32_watchdog watchdog_hw;
static atomic_t rs485_backend_ready;

static struct bms_app_config applied_config;
static struct bms_app_config staged_config;
static struct bms_config_store config_store;

#if defined(CONFIG_BMS_STM32_USE_NVS)
static struct bms_config_store_nvs nvs_backend;
#else
static struct bms_config_store_virtual ram_store;
#endif

static int capture_persistent(struct bms_persistent_data *data)
{
    memset(data, 0, sizeof(*data));
    data->config = applied_config;
    int ret = bms_core_runtime_export_estimator_persisted_state(
        &runtime, &data->estimator_state);
    if (ret == 0) data->estimator_state_valid = true;
    return 0;
}

static int save_config_now(void)
{
    struct bms_persistent_data data;
    int ret = capture_persistent(&data);
    if (ret == 0) ret = bms_config_store_save(&config_store, &data);
    return ret;
}

static int apply_staged(void)
{
    int ret = bms_config_validate(&staged_config);
    if (ret) return ret;

    bool supervisor_ready = supervisor.initialized;
    if (supervisor_ready) (void)bms_system_set_maintenance(&supervisor, true);

    bool mrun = atomic_get(&measurement.running) != 0;
    bool rrun = atomic_get(&runtime.running) != 0;
    if (rrun) (void)bms_core_runtime_stop(&runtime);
    if (mrun) (void)bms_measurement_stop(&measurement);

    ret = bms_measurement_set_period(&measurement,
                                     staged_config.measurement_period_ms);
    if (!ret) ret = bms_core_runtime_apply_configs(
        &runtime, &staged_config.protection, &staged_config.balancing,
        &staged_config.estimator, true);
    if (!ret) ret = bms_core_runtime_set_period(&runtime,
                                                 staged_config.runtime_period_ms);
    if (!ret) applied_config = staged_config;

    if (!ret && mrun) ret = bms_measurement_start(&measurement);
    if (!ret && rrun) ret = bms_core_runtime_start(&runtime);
    if (supervisor_ready) (void)bms_system_set_maintenance(&supervisor, false);
    return ret;
}

static int comm_get_applied(void *ctx, struct bms_app_config *out)
{ ARG_UNUSED(ctx); if (!out) return -EINVAL; *out = applied_config; return 0; }
static int comm_stage(void *ctx, const struct bms_app_config *in)
{ ARG_UNUSED(ctx); if (!in) return -EINVAL; int r=bms_config_validate(in); if(!r) staged_config=*in; return r; }
static int comm_apply(void *ctx) { ARG_UNUSED(ctx); return apply_staged(); }
static int comm_save(void *ctx) { ARG_UNUSED(ctx); return save_config_now(); }
static const struct bms_comm_config_ops comm_config_ops = {
    .get_applied=comm_get_applied, .stage=comm_stage,
    .apply=comm_apply, .save=comm_save,
};

static int init_config_store(struct bms_persistent_data *saved, bool *have_saved)
{
    *have_saved = false;
#if defined(CONFIG_BMS_STM32_USE_NVS)
    int ret = bms_config_store_nvs_init(&nvs_backend);
    if (ret) return ret;
    ret = bms_config_store_init(&config_store, &bms_config_store_nvs_ops, &nvs_backend);
#else
    bms_config_store_virtual_init(&ram_store);
    int ret = bms_config_store_init(&config_store, &bms_config_store_virtual_ops, &ram_store);
#endif
    if (ret) return ret;

    ret = bms_config_store_load(&config_store, saved);
    if (ret == 0) {
        applied_config = saved->config;
        staged_config = applied_config;
        *have_saved = true;
        return 0;
    }
    if (ret == -ENOENT) return 0;
    return ret;
}

static int init_monitors_and_topology(void)
{
    int ret = bms_monitor_bq76952_init(&bq0, "bq0", 0U);
    if (ret) return ret;

#if defined(CONFIG_BMS_PACK_32S)
    ret = bms_monitor_bq76952_init(&bq1, "bq1", 1U);
    if (ret) return ret;
    segments[0] = (struct bms_topology_segment_cfg){ &bq0.monitor, 0U, 16U, 0U };
    segments[1] = (struct bms_topology_segment_cfg){ &bq1.monitor, 0U, 16U, 16U };
    topology_cfg = (struct bms_topology_config){
        .name = "starz-stm32-32s",
        .segments = segments,
        .segment_count = 2U,
        .current_provider = &bq0.monitor,
        .pack_voltage_provider = NULL, /* summed stack voltage fallback */
        .switch_provider = &bq1.monitor, /* high-side FET driver in stacked architecture */
    };
#else
    segments[0] = (struct bms_topology_segment_cfg){ &bq0.monitor, 0U, 16U, 0U };
    topology_cfg = (struct bms_topology_config){
        .name = "starz-stm32-16s",
        .segments = segments,
        .segment_count = 1U,
        .current_provider = &bq0.monitor,
        .pack_voltage_provider = &bq0.monitor,
        .switch_provider = &bq0.monitor,
    };
#endif
    return bms_topology_init(&topology, &topology_cfg);
}

static int bq_device_number(uint8_t instance, uint16_t *number)
{
    struct bms_monitor_bq76952 *m = NULL;
    if (instance == 0U) m = &bq0;
#if defined(CONFIG_BMS_PACK_32S)
    else if (instance == 1U) m = &bq1;
#endif
    else return -EINVAL;

    uint8_t raw[2]; size_t n=0;
    int ret=bq76952_subcmd_read(&m->bq.transport, BQ_SUBCMD_DEVICE_NUMBER,
                                raw,sizeof(raw),&n);
    if (ret) return ret;
    if (n != 2U) return -EPROTO;
    *number = (uint16_t)raw[0] | ((uint16_t)raw[1]<<8);
    return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct bms_snapshot s;
    int ret=bms_measurement_get_latest(&measurement,&s);
    if(ret) return ret;
    shell_print(sh,"pack=%" PRId32 "mV current=%" PRId32 "mA cells=%u min=C%u/%" PRId32 " max=C%u/%" PRId32,
                s.pack_mv,s.current_ma,s.cell_count,s.min_cell_index+1U,s.min_cell_mv,
                s.max_cell_index+1U,s.max_cell_mv);
    shell_print(sh,"state=%s faults=0x%016" PRIx64 " balance=0x%08" PRIx64,
                bms_state_name(runtime.state_machine.state),s.faults,s.balance_mask);
    return 0;
}

static int cmd_system(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct bms_system_health h;
    int ret=bms_system_get_health(&supervisor,&h); if(ret) return ret;
    shell_print(sh,"mode=%s self_test=%s safe_latched=%u flags=0x%016" PRIx64,
                bms_system_mode_name(h.mode),h.self_test_passed?"PASS":"FAIL",
                h.safe_latched,h.flags);
    return 0;
}

static int cmd_board(const struct shell *sh, size_t argc, char **argv)
{ ARG_UNUSED(sh); ARG_UNUSED(argc); ARG_UNUSED(argv); return bms_board_stm32_print_status(); }

static int cmd_bq_ping(const struct shell *sh, size_t argc, char **argv)
{
    if(argc!=2U) return -EINVAL;
    unsigned long idx=strtoul(argv[1],NULL,0);
    uint16_t devnum=0; int ret=bq_device_number((uint8_t)idx,&devnum);
    if(!ret) shell_print(sh,"BQ%lu device_number=0x%04x",idx,devnum);
    return ret;
}

static int cmd_config_save(const struct shell *sh,size_t argc,char **argv)
{ ARG_UNUSED(argc); ARG_UNUSED(argv); int r=save_config_now(); if(!r) shell_print(sh,"configuration/state saved"); return r; }

static int cmd_events(const struct shell *sh,size_t argc,char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct bms_event_record rec[CONFIG_BMS_EVENT_LOG_CAPACITY]; size_t n=0;
    int ret=bms_events_copy(&runtime.events,rec,CONFIG_BMS_EVENT_LOG_CAPACITY,&n); if(ret)return ret;
    for(size_t i=0;i<n;++i) shell_print(sh,"#%" PRIu32 " snap=%" PRIu32 " %s data=0x%016" PRIx64 " detail=%" PRId32,
        rec[i].id,rec[i].snapshot_sequence,bms_event_type_name(rec[i].type),rec[i].data,rec[i].detail);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bms,
    SHELL_CMD(status,NULL,"Pack snapshot/runtime status",cmd_status),
    SHELL_CMD(system,NULL,"System supervisor status",cmd_system),
    SHELL_CMD(board,NULL,"Board GPIO/ALERT status",cmd_board),
    SHELL_CMD_ARG(bq_ping,NULL,"<0|1> read BQ device number",cmd_bq_ping,2,0),
    SHELL_CMD(config_save,NULL,"Save config + estimator state",cmd_config_save),
    SHELL_CMD(events,NULL,"Show event history",cmd_events),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(bms,&sub_bms,"STARZ BMS STM32 runtime",NULL);

static void rs485_rx_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        if (atomic_get(&rs485_backend_ready) != 0) {
            (void)bms_stm32_comm_poll_rs485(&comm_hw, &communications);
        }
        k_sleep(K_MSEC(1));
    }
}
K_THREAD_DEFINE(rs485_rx_tid, 2048, rs485_rx_thread,
                NULL,NULL,NULL, 5,0,0);

int main(void)
{
    printk("\nSTARZ BMS STM32 target starting...\n");
#if defined(CONFIG_BMS_PACK_32S)
    printk("Hardware profile: STM32F446 + 2x BQ76952, max 32S\n");
#else
    printk("Hardware profile: STM32F446 + 1x BQ76952, max 16S\n");
#endif

    bms_config_defaults(&applied_config);
    staged_config=applied_config;

    struct bms_persistent_data saved={0}; bool have_saved=false;
    int ret=init_config_store(&saved,&have_saved);
    if(ret){ printk("config store init/load failed: %d\n",ret); return 0; }

    ret=bms_board_stm32_init();
    if(ret){ printk("board init failed: %d\n",ret); return 0; }
    ret=init_monitors_and_topology();
    if(ret){ printk("BQ/topology init failed: %d\n",ret); return 0; }

    ret=bms_measurement_init(&measurement,&topology,applied_config.measurement_period_ms);
    if(ret){ printk("measurement init failed: %d\n",ret); return 0; }
    ret=bms_measurement_start(&measurement);
    if(ret){ printk("measurement start failed: %d\n",ret); return 0; }

    ret=bms_measurement_refresh_now(&measurement);
    if(ret){ printk("initial measurement failed: %d\n",ret); return 0; }

    ret=bms_core_runtime_init(&runtime,&measurement,&topology,
                              &applied_config.protection,&applied_config.balancing,
                              &applied_config.estimator,applied_config.runtime_period_ms,
                              IS_ENABLED(CONFIG_BMS_HW_ACTUATION_ENABLE));
    if(ret){ printk("runtime init failed: %d\n",ret); return 0; }
    if(have_saved && saved.estimator_state_valid)
        (void)bms_core_runtime_import_estimator_persisted_state(&runtime,&saved.estimator_state);
    ret=bms_core_runtime_start(&runtime);
    if(ret){ printk("runtime start failed: %d\n",ret); return 0; }

    ret=bms_stm32_comm_backend_init(&comm_hw);
    if(ret){ printk("comm hw init failed: %d\n",ret); return 0; }
    ret=bms_communications_init(&communications,&measurement,&runtime,
        &bms_stm32_can_ops,&comm_hw,&bms_stm32_rs485_ops,&comm_hw,
        &comm_config_ops,NULL,IS_ENABLED(CONFIG_BMS_REMOTE_CONFIG_WRITE),500U);
    if(ret){ printk("communications init failed: %d\n",ret); return 0; }
    ret=bms_communications_start(&communications);
    if(ret){ printk("communications start failed: %d\n",ret); return 0; }
    atomic_set(&rs485_backend_ready, 1);

    const struct bms_watchdog_backend_ops *wdt_ops=NULL; void *wdt_ctx=NULL;
#if defined(CONFIG_BMS_STM32_USE_HW_WATCHDOG)
    ret=bms_stm32_watchdog_init(&watchdog_hw,3000U);
    if(ret){ printk("watchdog init failed: %d\n",ret); return 0; }
    wdt_ops=&bms_stm32_watchdog_ops; wdt_ctx=&watchdog_hw;
#endif

    const struct bms_system_config syscfg={
        .supervisor_period_ms=250U,
        .max_snapshot_age_ms=applied_config.protection.max_snapshot_age_ms,
        .max_measurement_no_progress_ms=1200U,
        .max_runtime_no_progress_ms=1200U,
        .max_comm_no_progress_ms=2500U,
    };
    ret=bms_system_init(&supervisor,&measurement,&runtime,&communications,
        &topology,&config_store,&applied_config,wdt_ops,wdt_ctx,&syscfg);
    if(ret){ printk("system init failed: %d\n",ret); return 0; }
    ret=bms_system_run_self_test(&supervisor);
    if(ret){ printk("SELF-TEST FAILED: %d; outputs forced safe\n",ret); return 0; }
    ret=bms_system_start(&supervisor);
    if(ret){ printk("system start failed: %d\n",ret); return 0; }

    printk("Hardware actuation: %s\n",
           IS_ENABLED(CONFIG_BMS_HW_ACTUATION_ENABLE) ? "ENABLED" : "DISABLED (bring-up safe mode)");
    printk("STARZ BMS ready. Try: bms status, bms system, bms bq_ping 0\n");
#if defined(CONFIG_BMS_PACK_32S)
    printk("32S: also try bms bq_ping 1\n");
#endif
    while(1) k_sleep(K_SECONDS(60));
    return 0;
}
