#ifndef BQ76952_H_
#define BQ76952_H_

#include <stdbool.h>
#include <stdint.h>
#include "bq76952_transport.h"

struct bq76952_device {
    struct bq76952_transport transport;
    uint8_t cell_count;
    uint16_t user_volt_lsb_mv;
    uint32_t user_amp_lsb_ua;
};

struct bq76952_measurements {
    uint8_t cell_count;
    int16_t cell_mv[16];
    int32_t stack_mv;
    int32_t pack_mv;
    int32_t ld_mv;
    int32_t current_ma;
};

struct bq76952_faults {
    uint8_t safety_alert_a, safety_alert_b, safety_alert_c;
    uint8_t safety_status_a, safety_status_b, safety_status_c;
    uint8_t pf_status_a, pf_status_b, pf_status_c, pf_status_d;
    uint64_t flags;
};

struct bq76952_fet_state {
    uint8_t raw;
    bool chg, pchg, dsg, pdsg;
};

int bq76952_init(struct bq76952_device *, uint8_t, uint8_t, uint16_t, uint32_t);
int bq76952_read_all_measurements(struct bq76952_device *, struct bq76952_measurements *);
int bq76952_read_faults(struct bq76952_device *, struct bq76952_faults *);
int bq76952_read_fet_state(struct bq76952_device *, struct bq76952_fet_state *);
int bq76952_get_balance_mask(struct bq76952_device *, uint16_t *);
int bq76952_set_balance_mask(struct bq76952_device *, uint16_t);
int bq76952_force_all_off(struct bq76952_device *);
int bq76952_force_charge_off(struct bq76952_device *);
int bq76952_force_discharge_off(struct bq76952_device *);
int bq76952_release_fets(struct bq76952_device *);

#endif
