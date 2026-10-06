#ifndef BMS_CONFIG_STORE_H_
#define BMS_CONFIG_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include "bms_config.h"

#define BMS_CONFIG_STORE_MAGIC          0x434D5342U /* "BMSC" LE */
#define BMS_CONFIG_STORE_FORMAT_VERSION 1U
#define BMS_CONFIG_STORE_MAX_IMAGE_SIZE 256U

struct bms_config_store_backend_ops {
    int (*read)(void *context, uint8_t *data, size_t capacity, size_t *out_len);
    int (*write)(void *context, const uint8_t *data, size_t len);
    int (*erase)(void *context);
};

struct bms_config_store {
    const struct bms_config_store_backend_ops *ops;
    void *backend_context;
    uint32_t last_generation;
};

int bms_config_store_init(
    struct bms_config_store *,
    const struct bms_config_store_backend_ops *,
    void *backend_context);

int bms_config_store_save(struct bms_config_store *,
                          const struct bms_persistent_data *);

int bms_config_store_load(struct bms_config_store *,
                          struct bms_persistent_data *);

int bms_config_store_erase(struct bms_config_store *);

uint32_t bms_config_store_last_generation(
    const struct bms_config_store *);

/*
 * Native-sim virtual non-volatile backend.
 *
 * It survives application-level "simulated reboot" reinitialization inside one
 * native_sim process. It intentionally does NOT claim persistence after the
 * host executable itself exits.
 *
 * The real STM32 backend later maps these same read/write/erase operations to
 * Zephyr Settings/NVS/flash.
 */
struct bms_config_store_virtual {
    uint8_t image[BMS_CONFIG_STORE_MAX_IMAGE_SIZE];
    size_t image_len;
    bool valid;
};

void bms_config_store_virtual_init(struct bms_config_store_virtual *);

extern const struct bms_config_store_backend_ops
    bms_config_store_virtual_ops;

#endif
