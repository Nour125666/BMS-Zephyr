#include <errno.h>
#include "bms/bms_monitor.h"

static int validate(struct bms_monitor *m)
{
    return (m && m->ops && m->backend) ? 0 : -EINVAL;
}

bool bms_monitor_has_capability(const struct bms_monitor *m,
                                enum bms_monitor_capability cap)
{
    return m && ((m->capabilities & (uint32_t)cap) != 0U);
}

int bms_monitor_read_sample(struct bms_monitor *m, struct bms_monitor_sample *s)
{
    if (validate(m) || !s) return -EINVAL;
    return m->ops->read_sample ? m->ops->read_sample(m, s) : -ENOTSUP;
}

int bms_monitor_read_faults(struct bms_monitor *m, struct bms_monitor_fault_state *f)
{
    if (validate(m) || !f) return -EINVAL;
    return m->ops->read_faults ? m->ops->read_faults(m, f) : -ENOTSUP;
}

int bms_monitor_read_switch_state(struct bms_monitor *m,
                                  struct bms_monitor_switch_state *s)
{
    if (validate(m) || !s) return -EINVAL;
    return m->ops->read_switch_state ? m->ops->read_switch_state(m, s) : -ENOTSUP;
}

int bms_monitor_set_switch(struct bms_monitor *m,
                           enum bms_monitor_switch_request req)
{
    if (validate(m)) return -EINVAL;
    return m->ops->set_switch ? m->ops->set_switch(m, req) : -ENOTSUP;
}

int bms_monitor_get_balance_mask(struct bms_monitor *m, uint64_t *mask)
{
    if (validate(m) || !mask) return -EINVAL;
    return m->ops->get_balance_mask ? m->ops->get_balance_mask(m, mask) : -ENOTSUP;
}

int bms_monitor_set_balance_mask(struct bms_monitor *m, uint64_t mask)
{
    if (validate(m)) return -EINVAL;
    return m->ops->set_balance_mask ? m->ops->set_balance_mask(m, mask) : -ENOTSUP;
}
