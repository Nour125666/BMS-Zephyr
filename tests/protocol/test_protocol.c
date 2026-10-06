#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bms/bms_config.h"
#include "bms/bms_protocol.h"

int main(void)
{
    struct bms_telemetry_view v;
    memset(&v, 0, sizeof(v));

    v.snapshot_sequence = 42;
    v.cell_count = 32;
    v.pack_mv = 105600;
    v.current_ma = -25000;
    v.min_cell_mv = 3200;
    v.max_cell_mv = 3350;
    v.min_cell_index = 16;
    v.max_cell_index = 4;
    v.faults = 0x1122334455667788ULL;
    v.balance_mask = 0x00010001ULL;
    v.state = 3U; /* BMS_STATE_DISCHARGE_BLOCKED */
    v.charge_allowed = true;
    v.discharge_allowed = false;
    v.charge_switch_on = true;
    v.discharge_switch_on = false;
    v.soc_centi_pct = 7350;
    v.soh_centi_pct = 9200;
    v.estimated_full_capacity_mAh = 92000;
    v.equivalent_full_cycles_milli = 12345;

    for (unsigned i = 0; i < 32; ++i) {
        v.cell_mv[i] = 3300 + (int)i;
    }

    struct bms_can_frame frames[BMS_CAN_MAX_FRAMES];
    size_t frame_count = 0;

    assert(bms_protocol_build_can_frames(
        &v, frames, BMS_CAN_MAX_FRAMES, &frame_count) == 0);

    assert(frame_count == 13U);
    assert(frames[0].id == BMS_CAN_ID_PACK_STATUS);
    assert(frames[5].id == BMS_CAN_CELL_FRAME_BASE);
    assert(frames[12].id == BMS_CAN_CELL_FRAME_BASE + 7U);

    uint8_t status_payload[BMS_RS485_MAX_PAYLOAD];
    size_t status_len = 0;

    assert(bms_protocol_encode_status_payload(
        &v, status_payload, sizeof(status_payload), &status_len) == 0);

    struct bms_rs485_frame tx = {
        .type = BMS_RS485_RSP_STATUS,
        .sequence = 77,
        .payload_len = (uint16_t)status_len,
    };
    memcpy(tx.payload, status_payload, status_len);

    uint8_t wire[BMS_RS485_MAX_FRAME_SIZE];
    size_t wire_len = 0;

    assert(bms_protocol_rs485_encode(
        &tx, wire, sizeof(wire), &wire_len) == 0);

    struct bms_rs485_frame rx;
    assert(bms_protocol_rs485_decode(wire, wire_len, &rx) == 0);
    assert(rx.type == BMS_RS485_RSP_STATUS);
    assert(rx.sequence == 77);

    struct bms_telemetry_view decoded;
    memset(&decoded, 0, sizeof(decoded));

    assert(bms_protocol_decode_status_payload(
        rx.payload, rx.payload_len, &decoded) == 0);

    assert(decoded.pack_mv == v.pack_mv);
    assert(decoded.current_ma == v.current_ma);
    assert(decoded.soc_centi_pct == v.soc_centi_pct);
    assert(decoded.faults == v.faults);

    /* Corruption must be rejected by CRC. */
    wire[wire_len - 3U] ^= 0x40U;
    assert(bms_protocol_rs485_decode(wire, wire_len, &rx) != 0);

    struct bms_app_config cfg;
    bms_config_defaults(&cfg);

    uint8_t config_payload[BMS_RS485_MAX_PAYLOAD];
    size_t config_len = 0;

    assert(bms_protocol_encode_config_payload(
        &cfg, config_payload, sizeof(config_payload), &config_len) == 0);

    struct bms_app_config cfg2;
    assert(bms_protocol_decode_config_payload(
        config_payload, config_len, &cfg2) == 0);

    assert(bms_config_equal(&cfg, &cfg2));

    puts("PASS: CAN + RS485 protocol host tests");
    return 0;
}
