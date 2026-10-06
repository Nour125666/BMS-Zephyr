#include <errno.h>
#include <string.h>
#include "virtual_bq_model.h"

static struct virtual_bq_model models[2];

static void init_one(struct virtual_bq_model *m, int16_t base)
{
    memset(m, 0, sizeof(*m));
    for (uint8_t i = 0; i < 16; ++i) m->cell_mv[i] = base + (int16_t)((i % 5U) * 3U);
    m->fet_status = (1U << 0) | (1U << 2);
    m->da_configuration = 0x06;
}

void virtual_bq_reset(void)
{
    init_one(&models[0], 3300);
    init_one(&models[1], 3350);
}

struct virtual_bq_model *virtual_bq_get(uint8_t id)
{
    return (id < 2U) ? &models[id] : NULL;
}

int virtual_bq_set_cell(uint8_t id, uint8_t cell, int16_t mv)
{
    struct virtual_bq_model *m = virtual_bq_get(id);
    if (!m || cell >= 16U) return -EINVAL;
    m->cell_mv[cell] = mv;
    return 0;
}

int virtual_bq_set_current(uint8_t id, int32_t ma)
{
    struct virtual_bq_model *m = virtual_bq_get(id);
    if (!m) return -EINVAL;
    m->current_ma = ma;
    return 0;
}

int virtual_bq_set_uv(uint8_t id, bool on)
{
    struct virtual_bq_model *m = virtual_bq_get(id);
    if (!m) return -EINVAL;
    if (on) {
        m->safety_alert_a |= (1U << 2);
        m->safety_status_a |= (1U << 2);
    } else {
        m->safety_alert_a &= (uint8_t)~(1U << 2);
        m->safety_status_a &= (uint8_t)~(1U << 2);
    }
    return 0;
}
