#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

#include "bms_config_store_nvs.h"

#define STORAGE_NODE DT_NODELABEL(storage_partition)
#define RECORD_ID 1U

static int nvs_read_backend(void *context, uint8_t *data,
                            size_t capacity, size_t *out_len)
{
    struct bms_config_store_nvs *n = context;
    if (n == NULL || data == NULL || out_len == NULL) return -EINVAL;
    ssize_t ret = nvs_read(&n->fs, RECORD_ID, data, capacity);
    if (ret == -ENOENT) return -ENOENT;
    if (ret < 0) return (int)ret;
    *out_len = (size_t)ret;
    return 0;
}

static int nvs_write_backend(void *context, const uint8_t *data, size_t len)
{
    struct bms_config_store_nvs *n = context;
    if (n == NULL || data == NULL || len == 0U) return -EINVAL;
    ssize_t ret = nvs_write(&n->fs, RECORD_ID, data, len);
    return ret < 0 ? (int)ret : 0;
}

static int nvs_erase_backend(void *context)
{
    struct bms_config_store_nvs *n = context;
    if (n == NULL) return -EINVAL;
    return nvs_delete(&n->fs, RECORD_ID);
}

const struct bms_config_store_backend_ops bms_config_store_nvs_ops = {
    .read = nvs_read_backend,
    .write = nvs_write_backend,
    .erase = nvs_erase_backend,
};

int bms_config_store_nvs_init(struct bms_config_store_nvs *n)
{
    if (n == NULL) return -EINVAL;
    memset(n, 0, sizeof(*n));

    n->fs.flash_device = FIXED_PARTITION_DEVICE(STORAGE_NODE);
    n->fs.offset = FIXED_PARTITION_OFFSET(STORAGE_NODE);
    if (!device_is_ready(n->fs.flash_device)) return -ENODEV;

    struct flash_pages_info info;
    int ret = flash_get_page_info_by_offs(n->fs.flash_device,
                                          n->fs.offset, &info);
    if (ret) return ret;

    n->fs.sector_size = info.size;
    n->fs.sector_count = FIXED_PARTITION_SIZE(STORAGE_NODE) / info.size;
    if (n->fs.sector_count < 2U) return -ENOSPC;
    return nvs_mount(&n->fs);
}
