# Communications architecture — v0.8

The communications layer is deliberately above the BMS core.

```text
                        published snapshot
                              ↓
                      core runtime state
                              ↓
                       telemetry view
                         /         \
                        /           \
                reference CAN      RS485 frame protocol
                    ↓                   ↓
                CAN backend          RS485 backend
                    ↓                   ↓
                 hardware             hardware
```

Neither CAN nor RS485 reads the BQ directly.

## One coherent telemetry view

`bms_communications_collect_view()` combines:

- latest published snapshot
- latest protection decision
- authoritative state machine state
- estimator SOC/SOH/capacity counters

The transport protocols encode that already-stable BMS state.

## CAN

v0.8 defines a **project reference CAN telemetry profile** with IDs in the
0x500 range.

It is intentionally not advertised as Pylontech, Victron, J1939, CANopen, or
another vendor protocol.

The profile carries:

- pack voltage/current
- SOC/state/permissions
- cell min/max
- generic faults
- balance mask
- SOH/capacity/EFC
- all cell voltages in four-cell chunks

A real inverter-specific CAN profile can later be another encoder consuming the
same `bms_telemetry_view`.

## RS485

The project RS485 frame is:

```text
SOF 0xA5 0x5A
protocol version
message type
sequence
payload length
payload
CRC16-CCITT
```

It supports requests for:

- status
- all cells
- applied configuration

It also has controlled configuration operations:

- stage a complete validated config
- apply staged config
- save applied config

Remote config writes are a policy switch. They are enabled in native_sim only to
test the path. Production firmware should default them OFF unless an authentication
/ authorization requirement is explicitly designed.

## Transport backends

The communication service only knows:

```text
CAN backend: send(frame)
RS485 backend: send(bytes)
```

v0.8 provides native_sim virtual backends.

The future STM32/Zephyr backend will map:

```text
CAN backend   -> Zephyr CAN driver / can_send()
RS485 backend -> Zephyr UART + DE/RE GPIO
```

The protocol/core code does not change.

## Product-specific protocols remain separate

If the product must emulate a specific inverter/battery protocol, that profile is
not an STM32 concern. It is a product-level protocol adapter added beside the
reference CAN/RS485 codecs.
