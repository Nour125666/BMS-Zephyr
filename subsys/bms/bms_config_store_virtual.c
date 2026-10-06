#include <errno.h>
#include <string.h>

#include "bms/bms_config_store.h"

void bms_config_store_virtual_init(struct bms_config_store_virtual *backend)
{
    if (backend == NULL) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
}

static int virtual_read(void *context,
                        uint8_t *data,
                        size_t capacity,
                        size_t *out_len)
{
    struct bms_config_store_virtual *v = context;

    if (v == NULL || data == NULL || out_len == NULL) {
        return -EINVAL;
    }

    if (!v->valid) {
        return -ENOENT;
    }

    if (capacity < v->image_len) {
        return -ENOSPC;
    }

    memcpy(data, v->image, v->image_len);
    *out_len = v->image_len;
    return 0;
}

static int virtual_write(void *context,
                         const uint8_t *data,
                         size_t len)
{
    struct bms_config_store_virtual *v = context;

    if (v == NULL || data == NULL ||
        len == 0U || len > sizeof(v->image)) {
        return -EINVAL;
    }

    memcpy(v->image, data, len);
    v->image_len = len;
    v->valid = true;
    return 0;
}

static int virtual_erase(void *context)
{
    struct bms_config_store_virtual *v = context;

    if (v == NULL) {
        return -EINVAL;
    }

    memset(v, 0, sizeof(*v));
    return 0;
}

const struct bms_config_store_backend_ops
    bms_config_store_virtual_ops = {
        .read = virtual_read,
        .write = virtual_write,
        .erase = virtual_erase,
};
