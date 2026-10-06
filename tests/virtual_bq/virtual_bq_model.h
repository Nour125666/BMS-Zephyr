#ifndef VIRTUAL_BQ_MODEL_H_
#define VIRTUAL_BQ_MODEL_H_
#include <stdbool.h>
#include <stdint.h>

struct virtual_bq_model {
    int16_t cell_mv[16];
    int32_t current_ma;
    uint8_t safety_alert_a, safety_status_a;
    uint8_t safety_alert_b, safety_status_b;
    uint8_t safety_alert_c, safety_status_c;
    uint8_t pf_status_a, pf_status_b, pf_status_c, pf_status_d;
    uint8_t fet_status;
    uint16_t balance_mask;
    uint8_t da_configuration;
};

void virtual_bq_reset(void);
struct virtual_bq_model *virtual_bq_get(uint8_t);
int virtual_bq_set_cell(uint8_t, uint8_t, int16_t);
int virtual_bq_set_current(uint8_t, int32_t);
int virtual_bq_set_uv(uint8_t, bool);
#endif
