#ifndef BMS_PROTOCOL_H_
#define BMS_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bms_config.h"
#include "bms_snapshot.h"

#define BMS_PROTOCOL_VERSION 1U

#define BMS_CAN_MAX_FRAMES 16U
#define BMS_CAN_CELL_FRAME_BASE 0x510U

#define BMS_RS485_SOF0 0xA5U
#define BMS_RS485_SOF1 0x5AU
#define BMS_RS485_MAX_PAYLOAD 192U
#define BMS_RS485_MAX_FRAME_SIZE (2U + 1U + 1U + 2U + 2U + BMS_RS485_MAX_PAYLOAD + 2U)

/*
 * These CAN identifiers form the project's canonical/reference BMS protocol.
 * They are NOT a claim of compatibility with Pylontech, Victron, J1939,
 * CANopen, or any inverter vendor profile.
 */
enum bms_can_message_id {
    BMS_CAN_ID_PACK_STATUS = 0x500,
    BMS_CAN_ID_CELL_MINMAX = 0x501,
    BMS_CAN_ID_FAULTS_LOW  = 0x502,
    BMS_CAN_ID_FAULTS_HIGH = 0x503,
    BMS_CAN_ID_ESTIMATOR   = 0x504,
};

struct bms_can_frame {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
};

struct bms_telemetry_view {
    uint32_t snapshot_sequence;
    int64_t captured_at_ms;

    uint8_t cell_count;
    int32_t cell_mv[CONFIG_BMS_MAX_CELLS];

    int32_t pack_mv;
    int32_t current_ma;

    int32_t min_cell_mv;
    int32_t max_cell_mv;
    uint8_t min_cell_index;
    uint8_t max_cell_index;

    uint64_t faults;
    uint64_t balance_mask;

    uint8_t state; /* enum bms_operating_state numeric value */

    bool charge_allowed;
    bool discharge_allowed;

    bool charge_switch_on;
    bool discharge_switch_on;

    uint16_t soc_centi_pct;
    uint16_t soh_centi_pct;
    uint32_t estimated_full_capacity_mAh;
    uint32_t equivalent_full_cycles_milli;
};

/* Build all reference CAN telemetry frames for one coherent telemetry view. */
int bms_protocol_build_can_frames(
    const struct bms_telemetry_view *,
    struct bms_can_frame *frames,
    size_t max_frames,
    size_t *out_count);

enum bms_rs485_message_type {
    /* Requests */
    BMS_RS485_REQ_STATUS            = 0x10,
    BMS_RS485_REQ_CELLS             = 0x11,
    BMS_RS485_REQ_CONFIG            = 0x12,
    BMS_RS485_REQ_SET_STAGED_CONFIG = 0x20,
    BMS_RS485_REQ_APPLY_CONFIG      = 0x21,
    BMS_RS485_REQ_SAVE_CONFIG       = 0x22,

    /* Responses */
    BMS_RS485_RSP_STATUS = 0x90,
    BMS_RS485_RSP_CELLS  = 0x91,
    BMS_RS485_RSP_CONFIG = 0x92,
    BMS_RS485_RSP_ACK    = 0xA0,
    BMS_RS485_RSP_ERROR  = 0xA1,
};

struct bms_rs485_frame {
    uint8_t type;
    uint16_t sequence;
    uint16_t payload_len;
    uint8_t payload[BMS_RS485_MAX_PAYLOAD];
};

int bms_protocol_rs485_encode(
    const struct bms_rs485_frame *,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len);

int bms_protocol_rs485_decode(
    const uint8_t *data,
    size_t len,
    struct bms_rs485_frame *out);

int bms_protocol_encode_status_payload(
    const struct bms_telemetry_view *,
    uint8_t *out,
    size_t capacity,
    size_t *out_len);

int bms_protocol_decode_status_payload(
    const uint8_t *data,
    size_t len,
    struct bms_telemetry_view *out);

int bms_protocol_encode_cells_payload(
    const struct bms_telemetry_view *,
    uint8_t *out,
    size_t capacity,
    size_t *out_len);

int bms_protocol_decode_cells_payload(
    const uint8_t *data,
    size_t len,
    struct bms_telemetry_view *out);

int bms_protocol_encode_config_payload(
    const struct bms_app_config *,
    uint8_t *out,
    size_t capacity,
    size_t *out_len);

int bms_protocol_decode_config_payload(
    const uint8_t *data,
    size_t len,
    struct bms_app_config *out);

const char *bms_rs485_message_type_name(uint8_t type);

#endif
