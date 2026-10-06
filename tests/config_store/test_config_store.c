#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bms/bms_config.h"
#include "bms/bms_config_store.h"

int main(void)
{
    struct bms_app_config cfg;
    bms_config_defaults(&cfg);
    assert(bms_config_validate(&cfg) == 0);

    cfg.protection.cell_uv_trip_mv = 2700;
    cfg.protection.cell_uv_release_mv = 2900;
    cfg.measurement_period_ms = 250;
    cfg.runtime_period_ms = 125;

    struct bms_persistent_data in = {0};
    in.config = cfg;
    in.estimator_state_valid = true;
    in.estimator_state.remaining_charge_uAh = 73000000LL;
    in.estimator_state.estimated_full_capacity_mAh = 100000;
    in.estimator_state.charged_uAh = 123456;
    in.estimator_state.discharged_uAh = 654321;
    in.estimator_state.throughput_uAh = 777777;
    in.estimator_state.energy_in_uWh = 1000;
    in.estimator_state.energy_out_uWh = 2000;
    in.estimator_state.skipped_intervals = 3;

    struct bms_config_store_virtual nv;
    bms_config_store_virtual_init(&nv);

    struct bms_config_store store;
    assert(bms_config_store_init(&store,
                                 &bms_config_store_virtual_ops,
                                 &nv) == 0);

    assert(bms_config_store_save(&store, &in) == 0);
    assert(bms_config_store_last_generation(&store) == 1U);

    struct bms_persistent_data out;
    assert(bms_config_store_load(&store, &out) == 0);

    assert(bms_config_equal(&in.config, &out.config));
    assert(out.estimator_state_valid);
    assert(memcmp(&in.estimator_state,
                  &out.estimator_state,
                  sizeof(in.estimator_state)) == 0);

    /* Corrupt one byte: CRC must reject it. */
    assert(nv.image_len > 20U);
    nv.image[nv.image_len - 1U] ^= 0x5AU;
    assert(bms_config_store_load(&store, &out) != 0);

    assert(bms_config_store_erase(&store) == 0);
    assert(bms_config_store_load(&store, &out) != 0);

    puts("PASS: config + persistence store host tests");
    return 0;
}
