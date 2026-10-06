#ifndef BQ76952_TRANSPORT_H_
#define BQ76952_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#define BQ76952_TRANSFER_BUFFER_MAX 32U

struct bq76952_transport_config {
    bool crc_enabled;
    uint8_t retries;
    uint32_t poll_interval_us;
    uint32_t command_timeout_ms;
};

/*
 * One structure is shared by the native virtual backend and real Zephyr I2C
 * backend.  The virtual backend only uses instance_id; the real backend also
 * uses i2c/lock/cfg.
 */
struct bq76952_transport {
    uint8_t instance_id;
    struct i2c_dt_spec i2c;
    struct k_mutex lock;
    struct bq76952_transport_config cfg;
};

/*
 * Instance mapping:
 *   0 = bottom/single BQ (DT node label bq0 on STM32)
 *   1 = top BQ          (DT node label bq1 on 32S STM32)
 */
int bq76952_transport_init(struct bq76952_transport *, uint8_t instance_id);

int bq76952_direct_read(struct bq76952_transport *, uint8_t, void *, size_t);
int bq76952_direct_write(struct bq76952_transport *, uint8_t, const void *, size_t);
int bq76952_direct_read_u8(struct bq76952_transport *, uint8_t, uint8_t *);
int bq76952_direct_read_u16(struct bq76952_transport *, uint8_t, uint16_t *);
int bq76952_direct_read_i16(struct bq76952_transport *, uint8_t, int16_t *);
int bq76952_direct_write_u16(struct bq76952_transport *, uint8_t, uint16_t);

int bq76952_subcmd_command(struct bq76952_transport *, uint16_t);
int bq76952_subcmd_read(struct bq76952_transport *, uint16_t, void *, size_t, size_t *);
int bq76952_subcmd_write(struct bq76952_transport *, uint16_t, const void *, size_t);
int bq76952_dm_read(struct bq76952_transport *, uint16_t, void *, size_t);
int bq76952_dm_write(struct bq76952_transport *, uint16_t, const void *, size_t);

#endif
