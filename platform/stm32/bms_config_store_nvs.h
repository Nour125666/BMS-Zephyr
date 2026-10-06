#ifndef BMS_CONFIG_STORE_NVS_H_
#define BMS_CONFIG_STORE_NVS_H_

#include <zephyr/fs/nvs.h>
#include "bms/bms_config_store.h"

struct bms_config_store_nvs {
    struct nvs_fs fs;
};

int bms_config_store_nvs_init(struct bms_config_store_nvs *);
extern const struct bms_config_store_backend_ops bms_config_store_nvs_ops;

#endif
