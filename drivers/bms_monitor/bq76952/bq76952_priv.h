/*
 * Private BQ76952 driver definitions.
 *
 * "priv" = private. This header is for implementation files inside the
 * BQ76952 driver only. Application code and the generic BMS layer should
 * never include it.
 *
 * Keep internal state/helper definitions here when bq76952.c grows.
 */
#ifndef BQ76952_PRIV_H_
#define BQ76952_PRIV_H_

#include <stdint.h>

#include "bq76952_transport.h"

#define BQ76952_DEFAULT_I2C_ADDR_7BIT 0x08U
#define BQ76952_DEFAULT_RETRIES       2U
#define BQ76952_DEFAULT_POLL_US       200U
#define BQ76952_DEFAULT_TIMEOUT_MS    20U

/*
 * Future semantic-driver instance state can live here.
 * For now the transport object is the only mandatory mutable state.
 */
struct bq76952_runtime {
	struct bq76952_transport transport;

	/* Cached information can be added later, for example:
	 * uint16_t last_alarm_status;
	 * uint8_t  last_fet_status;
	 * bool     cfgupdate_active;
	 */
};

#endif /* BQ76952_PRIV_H_ */
