# STARZ BMS Zephyr v1.0 - DeepSeek Project Context

Use the supplied `starz-bms-zephyr-v1.0-final` source tree as the source of truth. This file is a compact handoff; the technical manual contains the full explanation.

## Goal

One reusable Zephyr BMS codebase supporting:

- native_sim: 32S virtual pack with 2 virtual BQ76952 monitors.
- STM32F446 16S: 1 physical BQ76952 on I2C1.
- STM32F446 32S: 2 stacked BQ76952 monitors, BQ0 direct I2C1 and BQ1 on I2C2 through ISO1640.
- Future ESP32 port by replacing platform backends, not rewriting BMS core logic.

## Architecture

```text
protocols / application
        ↓
system supervisor
        ↓
protection + balancing + estimator
        ↓
published coherent snapshot
        ↓
measurement
        ↓
topology
        ↓
generic bms_monitor API
        ↓
BQ76952 adapter
        ↓
BQ semantic driver
        ↓
transport (virtual or Zephyr I2C)
        ↓
hardware
```

Hard rule: below `bms_monitor` is hardware/vendor specific; above it is battery-domain logic.

## 16S topology

```text
BQ0 local 1..16 -> pack 1..16
current provider = BQ0
pack voltage provider = BQ0
switch/FET provider = BQ0
```

STM32 map:

```text
I2C1 PB8/PB9 -> BQ0 @ 0x08, 100 kHz
PC5          <- ALERT
PB14         -> RST_SHUT
PB6          -> DFETOFF
CAN1 PD0/PD1 -> 500 kbit/s
UART4 PC10/PC11 + PB2 DE/RE -> RS485 115200
UART5 PE8/PE7 -> ESP32 bridge 115200
USART3 PD8/PD9 -> console/GUI 115200
I2C3 PA8/PC9 -> LCD
HSE 8 MHz -> SYSCLK 168 MHz
```

## 32S topology

```text
BQ0 local 1..16 -> pack 1..16
BQ1 local 1..16 -> pack 17..32
current provider = BQ0
pack voltage provider = NULL -> topology sums BQ0.stack + BQ1.stack
switch/FET provider = BQ1
faults = aggregate both
balancing = global mask split into 2 local masks
```

Physical reference design:

```text
BQ0 @0x08 -> I2C1 PB8/PB9 direct
BQ1 @0x08 -> ISO1640 -> I2C2 PF1/PF0
```

Both use 0x08 because they are on separate buses. Proposed isolated upper GPIO assignments for a future PCB:

```text
PE12 <- TOP_ALERT
PB0  -> TOP_RST_SHUT
PB7  -> TOP_DFETOFF
```

These upper GPIO assignments are design requirements, not verified connections on an existing PCB.

## Core runtime

Measurement publishes one coherent `bms_snapshot`. Protection, balancing, estimator and protocols consume the published state; they do not independently read the BQ.

Default measurement period = 200 ms. Runtime period = 100 ms. Stale limit = 1000 ms.

Current convention: positive = charge, negative = discharge.

Protection states:

```text
INIT
NORMAL
CHARGE_BLOCKED
DISCHARGE_BLOCKED
ALL_BLOCKED
```

Generic switch requests:

```text
RELEASE
CHARGE_OFF
DISCHARGE_OFF
ALL_OFF
```

`RELEASE` means release host-forced inhibitions, not bypass BQ hardware protection.

## Development defaults - NOT production limits

```text
UV trip/release        2800 / 3000 mV
OV trip/release        3650 / 3500 mV
Charge OC trip/release 30000 / 25000 mA
Discharge OC magnitude 50000 / 40000 mA
Balancing start        3400 mV
Start/stop delta       20 / 10 mV
Balancing max |I|      5000 mA
Max balance cells      4
Design capacity        100000 mAh
Initial SOC            50%
```

## Estimator

SOC = coulomb counting from new snapshot sequence numbers only. Tracks charge, discharge, throughput, energy in/out and EFC. SOH is currently only:

```text
estimated usable full capacity / design capacity
```

There is NO automatic capacity-learning SOH algorithm yet.

## Configuration/persistence

Configuration is staged then applied. Persistent image has magic/version/length/generation/CRC32 and stores application config + retained estimator state.

Native store = RAM. STM32 NVS backend exists but `CONFIG_BMS_STM32_USE_NVS=n` by default until flash map approval.

## Communications

Reference CAN, not a vendor profile:

```text
0x500 pack status
0x501 min/max cell
0x502 low fault + low balance mask
0x503 high fault + high balance mask
0x504 estimator
0x510+ cell frames, 4 cells/frame
```

RS485 frame:

```text
A5 5A | version | type | seqLE16 | lenLE16 | payload | CRC16-CCITT
```

Remote config writes enabled in native_sim, disabled on STM32.

## System supervisor

Modes:

```text
BOOTING READY DEGRADED MAINTENANCE SAFE_LATCHED
```

Communication failures are degraded. Measurement/runtime failures are critical and latch safe. Safe latch stops periodic runtime, clears balancing and forces ALL_OFF. Recovery is explicit.

## STM32 safety defaults

`CONFIG_BMS_HW_ACTUATION_ENABLE=n` in both STM32 profiles. Do not change this until physical BQ/FET behavior is verified.

## Important current gaps

Do not hallucinate these as implemented:

- Generic temperature channels/protection/telemetry are not in the current snapshot model.
- Automatic SOH capacity learning is not implemented.
- 16S and 32S product initializers currently instantiate full 16-cell segments; 17-31S needs an explicit topology/cell-mode profile.
- Complete production BQ Data Memory programming/calibration is not yet integrated into the final STM32 startup.
- NVS is optional and disabled by default.
- CAN is a STARZ reference profile, not Pylontech/Victron/J1939/CANopen.
- 32S upper GPIO assignments are for a future PCB.
- Physical STM32 builds/electrical behavior still require target compilation and hardware validation.

## Build commands

```bash
# native_sim
west build -p always -b native_sim/native/64 . -- -DCONF_FILE=config/native_sim.conf

# STM32 16S
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_16s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_16s.overlay

# STM32 32S
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_32s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_32s.overlay
```

## Coding instructions for DeepSeek

1. Preserve the layer boundaries.
2. Do not add BQ register reads to protection/CAN/SOC/application code.
3. Add new hardware through transport/monitor/platform backends.
4. Add new product topology through topology configuration, not `if cell < 16` logic everywhere.
5. Protocols consume telemetry/snapshots, never hardware directly.
6. Current is never summed across stacked AFEs; use the configured current provider.
7. For 32S pack voltage, preserve stack-sum fallback unless a true full-pack provider is added.
8. Keep physical actuation disabled during bring-up.
9. Label all unvalidated thresholds and hardware assumptions clearly.
10. If adding temperature, extend generic monitor -> snapshot -> protection -> telemetry coherently.
11. If porting to ESP32, keep generic BMS core unchanged and replace only platform/backends where possible.
