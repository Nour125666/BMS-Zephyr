/*
 * BQ76952 Zephyr I2C transport
 *
 * Based on the BQ76952 transfer model:
 *   - direct commands use an 8-bit command address
 *   - subcommands/Data Memory use selector 0x3E/0x3F
 *   - transfer buffer is 0x40..0x5F
 *   - checksum/length are 0x60/0x61
 *
 * This file deliberately contains no battery policy.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "bq76952_priv.h"
#include "bq76952_reg.h"
#include "bq76952_transport.h"

#define BQ76952_CRC_POLY 0x07U

static uint8_t bq76952_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0U;

	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];

		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			if ((crc & 0x80U) != 0U) {
				crc = (uint8_t)((crc << 1) ^ BQ76952_CRC_POLY);
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

/* Transfer-buffer checksum: one's complement of the 8-bit sum. */
static uint8_t bq76952_buffer_checksum(uint16_t selector,
				       const uint8_t *data,
				       size_t len)
{
	uint8_t sum = (uint8_t)(selector & 0xFFU);
	sum = (uint8_t)(sum + (uint8_t)(selector >> 8));

	for (size_t i = 0; i < len; ++i) {
		sum = (uint8_t)(sum + data[i]);
	}

	return (uint8_t)~sum;
}

static int bq76952_i2c_write_retry(struct bq76952_transport *ctx,
				   const uint8_t *buf,
				   size_t len)
{
	int ret = -EIO;

	for (uint8_t attempt = 0U; attempt <= ctx->cfg.retries; ++attempt) {
		ret = i2c_write_dt(&ctx->i2c, buf, len);
		if (ret == 0) {
			return 0;
		}
		k_usleep(200);
	}

	return ret;
}

static int bq76952_i2c_write_read_retry(struct bq76952_transport *ctx,
					const uint8_t *write_buf,
					size_t write_len,
					uint8_t *read_buf,
					size_t read_len)
{
	int ret = -EIO;

	for (uint8_t attempt = 0U; attempt <= ctx->cfg.retries; ++attempt) {
		ret = i2c_write_read_dt(&ctx->i2c,
					write_buf, write_len,
					read_buf, read_len);
		if (ret == 0) {
			return 0;
		}
		k_usleep(200);
	}

	return ret;
}

/*
 * Register write without taking the mutex.
 *
 * CRC mode:
 *   first data-byte CRC covers:
 *     [address+W, register, first_data]
 *   every following data-byte CRC covers that data byte only.
 */
static int bq76952_reg_write_unlocked(struct bq76952_transport *ctx,
				      uint8_t reg,
				      const uint8_t *data,
				      size_t len)
{
	if ((data == NULL) || (len == 0U) ||
	    (len > BQ76952_TRANSFER_BUFFER_MAX)) {
		return -EINVAL;
	}

	if (!ctx->cfg.crc_enabled) {
		uint8_t tx[1U + BQ76952_TRANSFER_BUFFER_MAX];

		tx[0] = reg;
		memcpy(&tx[1], data, len);
		return bq76952_i2c_write_retry(ctx, tx, 1U + len);
	}

	uint8_t tx[1U + (2U * BQ76952_TRANSFER_BUFFER_MAX)];
	const uint8_t address_w = (uint8_t)(ctx->i2c.addr << 1);
	size_t p = 0U;

	tx[p++] = reg;

	for (size_t i = 0; i < len; ++i) {
		tx[p++] = data[i];

		if (i == 0U) {
			const uint8_t crc_input[3] = {
				address_w,
				reg,
				data[i],
			};
			tx[p++] = bq76952_crc8(crc_input, sizeof(crc_input));
		} else {
			tx[p++] = bq76952_crc8(&data[i], 1U);
		}
	}

	return bq76952_i2c_write_retry(ctx, tx, p);
}

/*
 * Register read without taking the mutex.
 *
 * CRC mode:
 *   first received-byte CRC covers:
 *     [address+W, register, address+R, first_data]
 *   every following CRC covers only its associated data byte.
 */
static int bq76952_reg_read_unlocked(struct bq76952_transport *ctx,
				     uint8_t reg,
				     uint8_t *data,
				     size_t len)
{
	if ((data == NULL) || (len == 0U) ||
	    (len > BQ76952_TRANSFER_BUFFER_MAX)) {
		return -EINVAL;
	}

	if (!ctx->cfg.crc_enabled) {
		return bq76952_i2c_write_read_retry(ctx,
						    &reg, 1U,
						    data, len);
	}

	uint8_t rx[2U * BQ76952_TRANSFER_BUFFER_MAX];
	const uint8_t address_w = (uint8_t)(ctx->i2c.addr << 1);
	const uint8_t address_r = (uint8_t)(address_w | 0x01U);

	int ret = bq76952_i2c_write_read_retry(ctx,
					       &reg, 1U,
					       rx, 2U * len);
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < len; ++i) {
		const uint8_t value = rx[2U * i];
		const uint8_t received_crc = rx[(2U * i) + 1U];
		uint8_t expected_crc;

		if (i == 0U) {
			const uint8_t crc_input[4] = {
				address_w,
				reg,
				address_r,
				value,
			};
			expected_crc = bq76952_crc8(crc_input,
						    sizeof(crc_input));
		} else {
			expected_crc = bq76952_crc8(&value, 1U);
		}

		if (received_crc != expected_crc) {
			return -EBADMSG;
		}

		data[i] = value;
	}

	return 0;
}

static int bq76952_wait_selector_echo_unlocked(struct bq76952_transport *ctx,
						uint16_t selector)
{
	const uint8_t want_lo = (uint8_t)(selector & 0xFFU);
	const uint8_t want_hi = (uint8_t)(selector >> 8);
	const int64_t deadline = k_uptime_get() + ctx->cfg.command_timeout_ms;

	while (k_uptime_get() <= deadline) {
		uint8_t echo[2];
		int ret = bq76952_reg_read_unlocked(ctx,
						    BQ76952_REG_CMD_LO,
						    echo,
						    sizeof(echo));
		if (ret == 0) {
			if ((echo[0] == want_lo) && (echo[1] == want_hi)) {
				return 0;
			}
		} else if ((ret != -EIO) && (ret != -EBADMSG)) {
			return ret;
		}

		k_usleep(ctx->cfg.poll_interval_us);
	}

	return -ETIMEDOUT;
}

/*
 * Read the complete transfer-buffer payload and validate the BQ transfer-buffer
 * checksum. Caller must already hold ctx->lock.
 */
static int bq76952_selector_read_unlocked(struct bq76952_transport *ctx,
					  uint16_t selector,
					  uint8_t *payload,
					  size_t payload_capacity,
					  size_t *payload_len)
{
	uint8_t selector_le[2];
	sys_put_le16(selector, selector_le);

	int ret = bq76952_reg_write_unlocked(ctx,
					     BQ76952_REG_CMD_LO,
					     selector_le,
					     sizeof(selector_le));
	if (ret != 0) {
		return ret;
	}

	/*
	 * The TRM explicitly recommends selector-echo polling for subcommands.
	 * It is also safe/useful for Data Memory because it prevents us from
	 * consuming a previous transfer-buffer response.
	 */
	ret = bq76952_wait_selector_echo_unlocked(ctx, selector);
	if (ret != 0) {
		return ret;
	}

	uint8_t total_length = 0U;
	ret = bq76952_reg_read_unlocked(ctx,
					BQ76952_REG_LENGTH,
					&total_length,
					1U);
	if (ret != 0) {
		return ret;
	}

	/* Length = payload + selector(2) + checksum/length(2). */
	if ((total_length < 4U) ||
	    (total_length > (BQ76952_TRANSFER_BUFFER_MAX + 4U))) {
		return -EPROTO;
	}

	const size_t actual_len = (size_t)total_length - 4U;
	uint8_t full_payload[BQ76952_TRANSFER_BUFFER_MAX];

	if (actual_len > 0U) {
		ret = bq76952_reg_read_unlocked(ctx,
						BQ76952_REG_TRANSFER_BUFFER,
						full_payload,
						actual_len);
		if (ret != 0) {
			return ret;
		}
	}

	uint8_t received_checksum = 0U;
	ret = bq76952_reg_read_unlocked(ctx,
					BQ76952_REG_CHECKSUM,
					&received_checksum,
					1U);
	if (ret != 0) {
		return ret;
	}

	const uint8_t expected_checksum =
		bq76952_buffer_checksum(selector, full_payload, actual_len);

	if (received_checksum != expected_checksum) {
		return -EBADMSG;
	}

	if (payload_len != NULL) {
		*payload_len = actual_len;
	}

	if (actual_len > payload_capacity) {
		return -EMSGSIZE;
	}

	if ((actual_len > 0U) && (payload != NULL)) {
		memcpy(payload, full_payload, actual_len);
	}

	return 0;
}

/*
 * Write selector + payload, then commit with checksum + total length.
 * Caller must already hold ctx->lock.
 */
static int bq76952_selector_write_unlocked(struct bq76952_transport *ctx,
					   uint16_t selector,
					   const uint8_t *payload,
					   size_t payload_len)
{
	if ((payload == NULL) || (payload_len == 0U) ||
	    (payload_len > BQ76952_TRANSFER_BUFFER_MAX)) {
		return -EINVAL;
	}

	uint8_t body[2U + BQ76952_TRANSFER_BUFFER_MAX];
	sys_put_le16(selector, body);
	memcpy(&body[2], payload, payload_len);

	int ret = bq76952_reg_write_unlocked(ctx,
					     BQ76952_REG_CMD_LO,
					     body,
					     2U + payload_len);
	if (ret != 0) {
		return ret;
	}

	const uint8_t commit[2] = {
		bq76952_buffer_checksum(selector, payload, payload_len),
		(uint8_t)(payload_len + 4U),
	};

	return bq76952_reg_write_unlocked(ctx,
					  BQ76952_REG_CHECKSUM,
					  commit,
					  sizeof(commit));
}

#define BQ0_NODE DT_NODELABEL(bq0)
#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
#define BQ1_NODE DT_NODELABEL(bq1)
#endif

static int transport_spec_for_instance(uint8_t instance_id,
                                       struct i2c_dt_spec *out,
                                       bool *crc_enabled)
{
    if (out == NULL || crc_enabled == NULL) {
        return -EINVAL;
    }

    switch (instance_id) {
    case 0: {
        static const struct i2c_dt_spec spec = I2C_DT_SPEC_GET(BQ0_NODE);
        *out = spec;
        *crc_enabled = DT_PROP_OR(BQ0_NODE, crc_enabled, false);
        return 0;
    }
#if DT_NODE_EXISTS(DT_NODELABEL(bq1))
    case 1: {
        static const struct i2c_dt_spec spec = I2C_DT_SPEC_GET(BQ1_NODE);
        *out = spec;
        *crc_enabled = DT_PROP_OR(BQ1_NODE, crc_enabled, false);
        return 0;
    }
#endif
    default:
        return -ENODEV;
    }
}

int bq76952_transport_init(struct bq76952_transport *ctx,
                           uint8_t instance_id)
{
    if (ctx == NULL) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->instance_id = instance_id;

    bool crc_enabled = false;
    int ret = transport_spec_for_instance(instance_id,
                                          &ctx->i2c,
                                          &crc_enabled);
    if (ret != 0) {
        return ret;
    }

    if (!device_is_ready(ctx->i2c.bus)) {
        return -ENODEV;
    }

    ctx->cfg.crc_enabled = crc_enabled;
    ctx->cfg.retries = BQ76952_DEFAULT_RETRIES;
    ctx->cfg.poll_interval_us = BQ76952_DEFAULT_POLL_US;
    ctx->cfg.command_timeout_ms = BQ76952_DEFAULT_TIMEOUT_MS;

    k_mutex_init(&ctx->lock);
    return 0;
}

int bq76952_direct_read(struct bq76952_transport *ctx,
			uint8_t command,
			void *data,
			size_t len)
{
	if ((ctx == NULL) || (data == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_reg_read_unlocked(ctx, command, data, len);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

int bq76952_direct_write(struct bq76952_transport *ctx,
			 uint8_t command,
			 const void *data,
			 size_t len)
{
	if ((ctx == NULL) || (data == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_reg_write_unlocked(ctx,
					     command,
					     (const uint8_t *)data,
					     len);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

int bq76952_direct_read_u8(struct bq76952_transport *ctx,
			   uint8_t command,
			   uint8_t *value)
{
	return bq76952_direct_read(ctx, command, value, 1U);
}

int bq76952_direct_read_u16(struct bq76952_transport *ctx,
			    uint8_t command,
			    uint16_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	uint8_t raw[2];
	int ret = bq76952_direct_read(ctx, command, raw, sizeof(raw));
	if (ret == 0) {
		*value = sys_get_le16(raw);
	}
	return ret;
}

int bq76952_direct_read_i16(struct bq76952_transport *ctx,
			    uint8_t command,
			    int16_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	uint16_t raw;
	int ret = bq76952_direct_read_u16(ctx, command, &raw);
	if (ret == 0) {
		*value = (int16_t)raw;
	}
	return ret;
}

int bq76952_direct_write_u16(struct bq76952_transport *ctx,
			     uint8_t command,
			     uint16_t value)
{
	uint8_t raw[2];
	sys_put_le16(value, raw);
	return bq76952_direct_write(ctx, command, raw, sizeof(raw));
}

int bq76952_subcmd_command(struct bq76952_transport *ctx,
			   uint16_t subcommand)
{
	if (ctx == NULL) {
		return -EINVAL;
	}

	uint8_t raw[2];
	sys_put_le16(subcommand, raw);

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_reg_write_unlocked(ctx,
					     BQ76952_REG_CMD_LO,
					     raw,
					     sizeof(raw));
	k_mutex_unlock(&ctx->lock);

	return ret;
}

int bq76952_subcmd_read(struct bq76952_transport *ctx,
			uint16_t subcommand,
			void *out,
			size_t out_capacity,
			size_t *out_len)
{
	if ((ctx == NULL) || (out_len == NULL) ||
	    ((out == NULL) && (out_capacity != 0U))) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_selector_read_unlocked(ctx,
						 subcommand,
						 (uint8_t *)out,
						 out_capacity,
						 out_len);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

int bq76952_subcmd_write(struct bq76952_transport *ctx,
			 uint16_t subcommand,
			 const void *payload,
			 size_t payload_len)
{
	if ((ctx == NULL) || (payload == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_selector_write_unlocked(ctx,
						  subcommand,
						  (const uint8_t *)payload,
						  payload_len);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

int bq76952_dm_read(struct bq76952_transport *ctx,
		    uint16_t address,
		    void *out,
		    size_t len)
{
	if ((ctx == NULL) || (out == NULL) ||
	    (len == 0U) || (len > BQ76952_TRANSFER_BUFFER_MAX)) {
		return -EINVAL;
	}

	uint8_t full[BQ76952_TRANSFER_BUFFER_MAX];
	size_t actual_len = 0U;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_selector_read_unlocked(ctx,
						 address,
						 full,
						 sizeof(full),
						 &actual_len);
	k_mutex_unlock(&ctx->lock);

	if (ret != 0) {
		return ret;
	}

	/*
	 * Data Memory readback normally returns a 32-byte block.
	 * The requested item starts at byte 0 of that block.
	 */
	if (actual_len < len) {
		return -EPROTO;
	}

	memcpy(out, full, len);
	return 0;
}

int bq76952_dm_write(struct bq76952_transport *ctx,
		     uint16_t address,
		     const void *data,
		     size_t len)
{
	if ((ctx == NULL) || (data == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->lock, K_FOREVER);
	int ret = bq76952_selector_write_unlocked(ctx,
						  address,
						  (const uint8_t *)data,
						  len);
	k_mutex_unlock(&ctx->lock);

	return ret;
}
