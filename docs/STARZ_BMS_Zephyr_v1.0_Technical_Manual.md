---
title: "STARZ BMS Zephyr v1.0"
subtitle: "Technical User Manual & Architecture Datasheet"
author: "STARZ Electronics / STARZ Energies"
date: "06 October 2026"
lang: en-US
---

# Document purpose

This document explains the complete `starz-bms-zephyr-v1.0-final` project as it exists in the supplied source tree. It is written for engineers who need to build, review, extend, port, or hand the project to another AI/code assistant without losing the architectural intent.

The source code is the authority. This manual describes what is implemented, what is intentionally disabled by default, and what still requires physical hardware validation. Development values are clearly separated from approved product values.

> **Safety boundary.** This project contains BMS control logic, but the STM32 profiles intentionally ship with physical FET/balancing actuation disabled. The software has not been electrically validated on the final 16S or 32S production hardware. Do not treat the supplied thresholds, GPIO assumptions, current calibration, isolation routing, or FET behavior as production-qualified until they are verified on the real board and battery system.

# 1. Executive summary

STARZ BMS Zephyr v1.0 is a reusable battery-management firmware platform built around a hardware-neutral BMS core and replaceable monitor/platform backends. The current implementation supports three build profiles from one source tree:

| Build profile | MCU / execution target | AFE arrangement | Logical pack profile |
|---|---|---|---|
| Native simulation | Zephyr `native_sim/native/64` | 2 virtual BQ76952 instances | 32 cells |
| STM32 16S | STM32F446ZE | 1 BQ76952 on I2C1 | 16 cells |
| STM32 32S | STM32F446ZE | 2 stacked BQ76952 devices | 32 cells |

The principal design rule is:

```text
Below bms_monitor: hardware/vendor specific.
Above bms_monitor: battery/BMS domain logic, hardware neutral.
```

The end-to-end architecture is:

```text
Application / shell / external protocols
                 |
          System supervisor
                 |
       +---------+----------+
       |         |          |
  Protection  Balancing  Estimator
       |         |          |
       +---------+----------+
                 |
        Published snapshot
                 |
           Measurement
                 |
            Topology
        +--------+--------+
        |                 |
     Monitor 0          Monitor 1
        |                 |
   BQ adapter          BQ adapter
        |                 |
  Semantic driver    Semantic driver
        |                 |
   Transport backend / Zephyr I2C
        |                 |
   BQ hardware or virtual model
```

The architecture is designed so the same BMS core can later be reused on ESP32 or another MCU by replacing the platform-specific backends and hardware description rather than rewriting protection, topology, estimation, balancing, configuration, and protocols.

# 2. Project scope and current status

## 2.1 Implemented

The final source tree contains:

- BQ76952 transport abstraction and a real Zephyr I2C backend.
- BQ76952 semantic driver for cell/stack/pack voltage, current, fault/status, FET state, balancing mask, and FET-control subcommands.
- Generic `bms_monitor` API independent of TI-specific register details.
- Topology layer for mapping one or more monitor segments into one logical pack.
- Coherent published snapshot model.
- Periodic measurement service and latest-snapshot cache.
- Software protection policy with hysteresis plus hardware-fault aggregation.
- BMS operating-state machine and generic switch requests.
- Automatic balancing policy.
- SOC coulomb counting, charge/energy counters, capacity-based SOH model, and equivalent full cycles.
- Validated application configuration with staged/applied semantics.
- Versioned persistent image with CRC-32.
- Native virtual persistence backend and optional STM32 NVS backend.
- Reference CAN telemetry protocol.
- Framed RS485 telemetry/configuration protocol with CRC16-CCITT.
- Native virtual CAN/RS485 backends and STM32 CAN/UART backends.
- Startup self-test, health supervisor, degraded state, safe latch, explicit recovery, and watchdog-feed policy.
- Native simulation fault injection and regression scenarios.
- STM32F446 16S and 32S Devicetree overlays.

## 2.2 Explicitly not completed or not validated

The following must not be assumed to be complete:

- Physical validation on the actual BMS PCB and battery pack.
- Final shunt/current calibration and voltage calibration.
- Real BQ76952 Data Memory product programming sequence for all protection, temperature, sleep, FET, and balancing settings.
- Automatic learned-capacity SOH algorithm.
- Generic temperature acquisition/protection in the current snapshot/policy model.
- Product-specific inverter CAN profile such as Pylontech, Victron, CANopen, or J1939.
- Final flash/NVS partition plan; NVS is disabled by default.
- Final 32S PCB implementation of isolated upper-AFE GPIO paths.
- Production authentication/authorization for remote configuration writes.
- Production qualification: EMC, abuse, short-circuit, thermal, vibration, safety certification, and long-term flash/endurance validation.

# 3. Source tree

The main project structure is:

```text
starz-bms-zephyr-v1.0-final/
|
|-- CMakeLists.txt
|-- Kconfig
|-- prj.conf
|
|-- config/
|   |-- native_sim.conf
|   |-- stm32_16s.conf
|   `-- stm32_32s.conf
|
|-- overlays/
|   |-- stm32f446_16s.overlay
|   |-- stm32f446_32s.overlay
|   `-- stm32f446_storage_nvs.overlay
|
|-- include/bms/
|   |-- bms_monitor.h
|   |-- bms_topology.h
|   |-- bms_snapshot.h
|   |-- bms_measurement.h
|   |-- bms_protection.h
|   |-- bms_balancing.h
|   |-- bms_state_machine.h
|   |-- bms_estimator.h
|   |-- bms_config.h
|   |-- bms_config_store.h
|   |-- bms_protocol.h
|   |-- bms_communications.h
|   |-- bms_events.h
|   |-- bms_system.h
|   `-- bms_system_policy.h
|
|-- subsys/bms/
|   `-- implementations of the generic BMS services
|
|-- drivers/bms_monitor/bq76952/
|   |-- bq76952.c
|   |-- bq76952_transport_i2c.c
|   |-- bms_monitor_bq76952.c
|   `-- BQ headers/register definitions
|
|-- platform/stm32/
|   |-- bms_board_stm32.c
|   |-- bms_communications_stm32.c
|   |-- bms_watchdog_stm32.c
|   `-- bms_config_store_nvs.c
|
|-- tests/virtual_bq/
|   |-- virtual_bq_model.c
|   `-- bq76952_transport_virtual.c
|
|-- tests/config_store/
|-- tests/protocol/
|-- tests/system/
|
|-- src/main_native.c
`-- src/main_stm32.c
```

The key separation is that `subsys/bms/` does not need STM32 register knowledge and the platform-specific code does not decide protection policy.

# 4. Build profiles and Kconfig

The root Kconfig defines two independent choices: platform and pack profile.

## 4.1 Platform selection

```text
CONFIG_BMS_PLATFORM_NATIVE_SIM
CONFIG_BMS_PLATFORM_STM32F446
```

## 4.2 Pack topology selection

```text
CONFIG_BMS_PACK_16S
CONFIG_BMS_PACK_32S
```

`CONFIG_BMS_MONITOR_MAX_CELLS` is 16 and `CONFIG_BMS_MAX_CELLS` is 32. `CONFIG_BMS_TOPOLOGY_MAX_MONITORS` defaults to 4, although the current product profiles instantiate one or two monitors.

## 4.3 Safety-related build switches

`CONFIG_BMS_HW_ACTUATION_ENABLE` is enabled by default only in `native_sim`. Both STM32 profiles explicitly set it to `n` so that the state machine and balancing policy can run without physically switching FETs or balancing cells during first bring-up.

`CONFIG_BMS_REMOTE_CONFIG_WRITE` is enabled by default in simulation and disabled on STM32.

`CONFIG_BMS_STM32_USE_HW_WATCHDOG` is enabled in both STM32 profiles.

`CONFIG_BMS_STM32_USE_NVS` is disabled by default pending approval of the final flash layout.

# 5. Core architectural contracts

## 5.1 Transport

The transport layer answers only: **how are bytes exchanged with the BQ76952?**

Two implementations exist:

- `tests/virtual_bq/bq76952_transport_virtual.c` for simulation.
- `drivers/bms_monitor/bq76952/bq76952_transport_i2c.c` for real Zephyr I2C.

The real transport obtains the BQ instance from Devicetree by instance ID and uses Zephyr `i2c_dt_spec` objects. It supports direct-register operations, subcommand/Data Memory transfer-buffer access, optional BQ CRC mode, retry, selector-echo polling, transfer length validation, and transfer-buffer checksum validation.

## 5.2 Semantic BQ driver

`bq76952.c` translates BQ register semantics into typed engineering values. It currently provides:

- 16 cell voltages.
- Stack voltage.
- BQ PACK and LD voltages.
- CC2 current.
- Safety Alert/Status A/B/C and PF Status A-D reads.
- Generic fault flag mapping for cell UV, cell OV, charge OC, discharge OC, short-circuit discharge, and permanent failure.
- FET-state decoding.
- Cell-balancing mask get/set.
- Generic host FET commands: all off, charge off, discharge off, release.

The current semantic driver initializes each monitor as 16 cells with voltage LSB 10 mV for stack/PACK/LD user-voltage commands and current scaling of 10 mA per raw current unit (`user_amp_lsb_ua = 10000`). These scaling assumptions must match the actual BQ Data Memory configuration on hardware.

## 5.3 Generic monitor API

`bms_monitor` is the vendor-neutral interface used by topology. It exposes capabilities rather than BQ-specific functions:

```text
CELL_VOLTAGE
STACK_VOLTAGE
PACK_VOLTAGE
CURRENT
LOCAL_PROTECTION
BALANCING
SWITCH_CONTROL
```

Operations are:

```text
read_sample
read_faults
read_switch_state
set_switch
get_balance_mask
set_balance_mask
```

This is the portability boundary. A future non-TI AFE should implement these operations without changing the BMS services above topology.

# 6. Topology: making multiple AFEs look like one battery

The topology layer maps monitor-local cell numbering to global pack cell numbering and assigns explicit providers for pack-level quantities.

A segment contains:

```text
monitor pointer
local cell offset
cell count
global cell offset
```

Topology verifies there are no overlapping or missing global cell positions.

## 6.1 16S profile

```text
BQ0 local cells 1..16 -> global cells 1..16
current provider        -> BQ0
pack-voltage provider   -> BQ0
switch/FET provider     -> BQ0
```

## 6.2 32S profile

```text
BQ0 local cells 1..16 -> global cells 1..16
BQ1 local cells 1..16 -> global cells 17..32

current provider        -> BQ0
pack-voltage provider   -> none
switch/FET provider     -> BQ1
```

Because no single stacked AFE is assumed to report the complete 32S pack voltage, the topology uses the sum of valid local stack voltages when `pack_voltage_provider == NULL`:

```text
logical pack voltage = BQ0 stack voltage + BQ1 stack voltage
```

Faults and alerts from local-protection-capable monitors are OR-aggregated. The global balancing mask is split into monitor-local masks and routed to the relevant monitor.

### Important profile limitation

The supplied 16S and 32S profiles are currently **fixed logical profiles**: one 16-cell segment or two 16-cell segments. The architecture can represent other counts, but a 19S/20S product would require changing the topology segment `cell_count` values and correctly configuring the BQ `VCellMode`/unused channels. Do not assume the current `stm32_32s.conf` automatically adapts to 17-31 cells.

# 7. Snapshot and measurement model

`struct bms_snapshot` is the coherent whole-pack observation consumed by upper layers. It contains:

- Sequence number and publish timestamp.
- Cell count and cell-voltage array.
- Cell-voltage sum.
- Stack voltage and validity.
- Pack voltage and validity.
- Current and validity.
- Minimum/maximum cell voltage and indices.
- Alerts, faults, and permanent-fail flag.
- Global balancing mask.
- Charge/discharge/precharge/predischarge switch state.

The measurement service owns acquisition and publication. It performs topology reads outside the cache mutex, then publishes a complete new snapshot under lock. Consumers read the **latest published snapshot**; they do not independently read BQs.

This avoids time-skew such as protection using one cell measurement while CAN reports a later current sample.

The default development timing is:

| Parameter | Default |
|---|---:|
| Measurement period | 200 ms |
| Core-runtime period | 100 ms |
| Stale-snapshot limit | 1000 ms |

# 8. Protection policy

Protection is a pure decision layer. It consumes a snapshot plus snapshot age and returns:

```text
snapshot_sequence
reason bitmap
charge_allowed
discharge_allowed
```

It does not directly control hardware.

## 8.1 Software reasons

| Reason | Meaning |
|---|---|
| `SW_CELL_UV` | Minimum cell at/below UV trip threshold |
| `SW_CELL_OV` | Maximum cell at/above OV trip threshold |
| `SW_CHARGE_OC` | Positive current above charge-OC threshold |
| `SW_DISCHARGE_OC` | Negative current magnitude above discharge-OC threshold |
| `STALE_SNAPSHOT` | Published measurement is older than allowed |

UV, OV, and overcurrent software protections have separate trip/release thresholds and therefore hysteresis.

## 8.2 Hardware-derived reasons

| Reason | Generic source |
|---|---|
| `HW_CELL_UV` | BQ generic UV fault flag |
| `HW_CELL_OV` | BQ generic OV fault flag |
| `HW_CHARGE_OC` | BQ charge OC flag |
| `HW_DISCHARGE_OC` | BQ discharge OC flag |
| `HW_SHORT_CIRCUIT` | BQ SCD flag |
| `HW_PERMANENT_FAIL` | BQ PF status |

## 8.3 Charge/discharge blocking matrix

Charge is blocked by software/hardware OV, charge overcurrent, stale snapshot, or permanent failure.

Discharge is blocked by software/hardware UV, discharge overcurrent, short circuit, stale snapshot, or permanent failure.

A stale snapshot blocks both directions as a fail-safe condition.

## 8.4 Development thresholds

These values are defaults in `bms_config.c`, **not approved product limits**:

| Setting | Trip | Release |
|---|---:|---:|
| Cell UV | 2800 mV | 3000 mV |
| Cell OV | 3650 mV | 3500 mV |
| Charge OC | +30000 mA | +25000 mA |
| Discharge OC magnitude | 50000 mA | 40000 mA |

# 9. Operating state machine and switch authority

The state machine converts the protection decision into an operating state:

```text
INIT
NORMAL
CHARGE_BLOCKED
DISCHARGE_BLOCKED
ALL_BLOCKED
```

The mapping is:

| Charge allowed | Discharge allowed | State | Generic switch request |
|---|---|---|---|
| Yes | Yes | `NORMAL` | `RELEASE` |
| No | Yes | `CHARGE_BLOCKED` | `CHARGE_OFF` |
| Yes | No | `DISCHARGE_BLOCKED` | `DISCHARGE_OFF` |
| No | No | `ALL_BLOCKED` | `ALL_OFF` |

`RELEASE` means **release host-forced FET inhibitions**. It does not mean force the FETs on against active BQ protection.

On physical STM32 builds, actual switch/balancing writes are gated by `CONFIG_BMS_HW_ACTUATION_ENABLE`. During safe bring-up this remains disabled even though decisions and state transitions continue to run.

# 10. Balancing policy

The balancing policy consumes the same published snapshot and protection decision. It calculates a global logical cell mask, independent of the number of monitors.

Balancing is inhibited when:

- The policy is disabled.
- Any protection reason is active or charge/discharge is blocked.
- Current is invalid.
- Absolute current is above the configured limit.
- Maximum cell voltage is below the start voltage.
- Cell delta is too small.

Default development values are:

| Setting | Default |
|---|---:|
| Start cell voltage | 3400 mV |
| Start delta above minimum | 20 mV |
| Stop delta | 10 mV |
| Maximum absolute pack current | 5000 mA |
| Maximum simultaneous cells | 4 |

The policy uses per-cell hysteresis: a previously active cell remains eligible until its delta falls to the stop threshold. When more cells are eligible than allowed, the highest-voltage cells are selected.

For 32S, topology converts the global 32-bit logical mask into separate 16-bit masks for BQ0 and BQ1.

# 11. Estimator: SOC, charge, energy and SOH model

## 11.1 Current convention

```text
positive current = charging
negative current = discharging
```

## 11.2 SOC

SOC is coulomb-counted from published current snapshots. Duplicate snapshot sequence numbers are ignored because the core runtime may execute faster than measurement publication.

Unexpectedly large timing gaps are not integrated; the estimator re-anchors rather than pretending to know the current during missing data.

Default simulator/initial configuration:

| Setting | Default |
|---|---:|
| Design capacity | 100000 mAh |
| Initial estimated full capacity | 100000 mAh |
| Initial SOC | 50.00% |
| Charge efficiency | 100.0% |
| Maximum integration interval | 1000 ms |

## 11.3 Counters

The estimator tracks:

- Remaining charge in uAh.
- Charged uAh.
- Discharged uAh.
- Total throughput uAh.
- Energy in uWh.
- Energy out uWh.
- Discharge-based equivalent full cycles.

## 11.4 SOH

Automatic capacity learning is not implemented. Current SOH is a capacity ratio:

```text
SOH = estimated usable full capacity / design capacity
```

The `estimated_full_capacity_mAh` value can be restored from persistence or set by a future learning/calibration algorithm without changing CAN/RS485/topology code.

# 12. Configuration and persistence

## 12.1 Application configuration

`struct bms_app_config` contains:

```text
schema version
measurement period
runtime period
protection configuration
balancing configuration
estimator configuration
```

Topology is intentionally not stored as user configuration. It is a product/hardware definition selected by build/Devicetree.

## 12.2 Staged versus applied configuration

Configuration edits are staged before activation:

```text
edit STAGED config
      |
      v
validate
      |
      v
bms config apply
      |
      v
APPLIED config
```

This prevents an incomplete multi-field edit from immediately changing safety policy.

## 12.3 Persistent format

The project does not write raw C structs directly to flash. It explicitly serializes a versioned image containing:

```text
magic
format version
payload length
generation counter
CRC-32
payload
```

The payload contains the application configuration and optional retained estimator state.

Retained estimator state includes remaining charge, estimated full capacity, charge/discharge/throughput counters, energy counters, and skipped-interval count. Timing anchors and fractional integration remainders are deliberately not persisted.

## 12.4 Backends

- Native simulation uses a RAM-backed virtual store that survives only within the current process.
- STM32 currently also defaults to the RAM backend.
- `bms_config_store_nvs.c` provides a real STM32 NVS backend, but it is disabled until the flash map is approved.

The optional reference overlay reserves `0x40000..0x7FFFF` for storage on a 512 KiB device. This must not be enabled blindly; application, bootloader, and linker usage must be reviewed first.

# 13. CAN reference protocol

The CAN profile is a STARZ project reference protocol. It is **not** Pylontech, Victron, J1939, CANopen, or another vendor profile.

For a 32-cell pack the encoder produces 13 frames per telemetry cycle: 5 fixed frames plus 8 cell-voltage frames.

## 13.1 Fixed frames

### CAN ID 0x500 - Pack status

| Bytes | Encoding |
|---|---|
| 0..1 | Pack voltage in 10 mV units, unsigned 16-bit |
| 2..3 | Current in 100 mA units, signed 16-bit |
| 4..5 | SOC in 0.1% units |
| 6 | Operating-state numeric value |
| 7 bit0 | Charge allowed |
| 7 bit1 | Discharge allowed |
| 7 bit2 | CHG switch state |
| 7 bit3 | DSG switch state |

### CAN ID 0x501 - Cell minimum/maximum

| Bytes | Encoding |
|---|---|
| 0..1 | Minimum cell voltage in mV |
| 2..3 | Maximum cell voltage in mV |
| 4 | Minimum-cell index, 1-based |
| 5 | Maximum-cell index, 1-based |
| 6 | Cell count |
| 7 | Reserved |

### CAN ID 0x502

Low 32 bits of generic fault bitmap plus low 32 bits of balance mask.

### CAN ID 0x503

High 32 bits of generic fault bitmap plus high 32 bits of balance mask.

### CAN ID 0x504 - Estimator

| Bytes | Encoding |
|---|---|
| 0..1 | SOH in 0.1% units |
| 2..3 | Estimated full capacity in 100 mAh units |
| 4..7 | Equivalent full cycles in milli-cycles |

## 13.2 Cell-voltage frames

Cell frames start at `0x510`. Each frame holds four 16-bit direct-mV cell values:

```text
0x510 -> cells 1..4
0x511 -> cells 5..8
...
0x517 -> cells 29..32
```

# 14. RS485 protocol

The RS485 protocol is framed as:

```text
SOF0       0xA5
SOF1       0x5A
version    1 byte
message    1 byte
sequence   2 bytes little-endian
length     2 bytes little-endian
payload    0..192 bytes
CRC16      2 bytes little-endian
```

CRC uses CRC16-CCITT-FALSE with polynomial `0x1021` and initial value `0xFFFF`. CRC covers the frame from protocol version through payload, excluding SOF and the CRC field itself.

## 14.1 Request types

| Value | Request |
|---:|---|
| 0x10 | Status |
| 0x11 | Cell voltages |
| 0x12 | Applied configuration |
| 0x20 | Set staged configuration |
| 0x21 | Apply staged configuration |
| 0x22 | Save configuration |

## 14.2 Response types

| Value | Response |
|---:|---|
| 0x90 | Status |
| 0x91 | Cells |
| 0x92 | Configuration |
| 0xA0 | ACK |
| 0xA1 | Error |

Remote configuration writes are allowed in native simulation for test coverage and disabled by default on STM32.

# 15. Events, system supervisor and watchdog

## 15.1 Event log

The in-RAM event ring stores up to 32 records by default. Event types include:

- Protection changed.
- State changed.
- Actuation error.
- Balance changed.
- Self-test.
- System mode changed.
- System safe-latched.
- System recovered.

Events are diagnostic history, not the safety source of truth.

## 15.2 System modes

```text
BOOTING
READY
DEGRADED
MAINTENANCE
SAFE_LATCHED
```

Communication-only faults are warnings and produce `DEGRADED`. Core battery-control failures are critical and produce `SAFE_LATCHED`.

## 15.3 Startup self-test

The system supervisor checks:

- Applied configuration validates.
- Topology exists and has a logical cell count.
- A fresh measurement can be acquired.
- Core protection/estimator processing works.
- Communication telemetry can be collected.
- Persistent storage is readable; an empty first-boot store is accepted.

## 15.4 Health supervision

The periodic supervisor detects:

- No snapshot.
- Stale measurement.
- Measurement error, stopped service, or no progress.
- Runtime error, stopped service, or no progress.
- Communication error, stopped service, or no progress.

## 15.5 Safe latch

When a critical condition enters `SAFE_LATCHED`:

```text
periodic core runtime stops
balance mask is cleared
switch command is forced to ALL_OFF
```

Recovery is explicit. The supervisor does not automatically release the battery after a transient critical failure.

## 15.6 Watchdog

The supervisor owns watchdog feeding. It feeds only when self-test passed and the system is not safe-latched. A communication-only degraded state does not suppress feeding because battery control can remain healthy.

On STM32 the backend uses the Zephyr watchdog API. On native simulation a virtual watchdog backend is used.

# 16. Native simulation profile

The native simulator is the main software regression environment. It instantiates two virtual BQ76952 devices and exercises the same semantic driver, monitor abstraction, topology, measurement, protection, balancing, estimator, configuration, protocols, and system supervisor used by the real target.

The virtual transport validates the BQ transport **contract**, not the electrical behavior of STM32 I2C.

## 16.1 Build

```bash
west build -p always -b native_sim/native/64 . -- \
  -DCONF_FILE=config/native_sim.conf
```

Find and run the executable, then connect to the PTY printed by `native_sim`.

## 16.2 Recommended acceptance commands

```text
bms test all
bms scenario all
bms system status
bms snapshot
```

`bms scenario all` covers normal operation, software UV, software OV, discharge OC, charge OC, hardware BQ UV propagation, balancing, CAN, and RS485 behavior.

## 16.3 Useful fault injection

```text
bms inject cell <bq 0|1> <cell 1..16> <mV>
bms inject current <bq 0|1> <mA>
bms inject uv <bq 0|1> on|off
```

In the 32S topology, BQ1 local cell 1 is global pack cell 17.

# 17. STM32F446 16S hardware profile

The 16S profile reconstructs the legacy STM32F446ZET6 hardware contract using Zephyr Devicetree and STM32 pinctrl.

## 17.1 MCU and clocks

- MCU: STM32F446ZE / STM32F446ZET6 family, LQFP144 in the legacy design.
- HSE: 8 MHz.
- PLL: M=4, N=168, P=2, Q=7.
- SYSCLK: 168 MHz.
- AHB prescaler: 1.
- APB1 prescaler: 4.
- APB2 prescaler: 2.

## 17.2 Peripheral map

| Function | STM32 peripheral / pins | Configuration |
|---|---|---|
| BQ76952 | I2C1 PB8/PB9 | 100 kHz, address 0x08 |
| BQ ALERT | PC5 | Input/interrupt |
| BQ RST_SHUT | PB14 | Output, inactive low |
| BQ DFETOFF | PB6 | Output, inactive low |
| CAN | CAN1 PD0/PD1 | 500 kbit/s |
| RS485 | UART4 PC10/PC11 | 115200 baud |
| RS485 DE/RE | PB2 | GPIO output |
| ESP32 bridge | UART5 PE8/PE7 | 115200 baud |
| Console / GUI | USART3 PD8/PD9 | 115200 baud |
| LCD | I2C3 PA8/PC9 | Standard-mode I2C |
| Watchdog | IWDG | Enabled in STM32 profile |

Additional documented board I/O includes user switches, SOC LEDs, START input, and buzzer.

The legacy CubeMX project configured PC5/ALERT and PF11/START inconsistently as outputs. The Zephyr profile follows the product hardware map and treats these as inputs.

# 18. STM32F446 32S reference hardware profile

The 32S profile is a new STARZ reference definition based on the 16S MCU mapping and TI stacked-BQ769x2 guidance.

## 18.1 Electrical topology

```text
Cells 1..16
    |
 BQ0 bottom
    |
 I2C1 PB8/PB9
    |
STM32F446

Cells 17..32
    |
 BQ1 top (floating upper-group ground)
    |
 ISO1640 isolated I2C
    |
 I2C2 PF1/PF0
    |
STM32F446
```

The bottom AFE shares MCU/pack-negative ground. The upper AFE is referenced to the upper group and therefore requires isolated or appropriately level-shifted communication.

## 18.2 Deliberate two-bus choice

Both BQ devices remain at 7-bit address `0x08` because they are on separate MCU I2C controllers:

```text
BQ0 -> I2C1 @ 0x08
BQ1 -> I2C2 @ 0x08 through ISO1640
```

This avoids irreversible OTP address programming purely to resolve an address collision.

## 18.3 Provider roles

- BQ0 provides current.
- BQ1 is the software switch/FET provider for the documented high-side stacked architecture.
- Logical pack voltage is the sum of BQ0 and BQ1 stack voltages.
- Faults are aggregated from both monitors.
- Balancing masks are split across both monitors.

## 18.4 Proposed upper-AFE GPIO routing

Because the final 32S PCB does not yet exist, these are **design assignments**, not verified legacy connections:

| Function | MCU pin | Requirement |
|---|---|---|
| TOP_ALERT | PE12 | Upper-to-MCU isolated/level-shifted input |
| TOP_RST_SHUT | PB0 | MCU-to-upper isolated/level-shifted output |
| TOP_DFETOFF | PB7 | MCU-to-upper isolated/level-shifted output |

The ISO1640 secondary-side power and any upper digital isolator supplies must be valid whenever upper-AFE communication/control is needed.

Fast hardware protection paths between the bottom current-monitoring AFE and the high-side FET-driving stage are a PCB design responsibility. Host software is not a substitute for a required fast SCD/OCD turn-off path.

# 19. STM32 runtime and first bring-up

The STM32 application initializes in this sequence:

```text
load defaults / optional saved config
board GPIO initialization
BQ monitor(s) + topology
measurement service
core runtime
CAN / RS485 backend
communications service
hardware watchdog
system supervisor
startup self-test
periodic supervisor
```

By default physical actuation is disabled.

## 19.1 STM32 16S build

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_16s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_16s.overlay
```

## 19.2 STM32 32S build

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_32s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_32s.overlay
```

The Nucleo board target supplies Zephyr's STM32F446ZE SoC/base definition; the project overlays define the STARZ application mapping.

## 19.3 First hardware checks

Use the small STM32 shell:

```text
bms status
bms system
bms board
bms bq_ping 0
bms events
```

On 32S:

```text
bms bq_ping 1
```

Do not enable `CONFIG_BMS_HW_ACTUATION_ENABLE` until both BQ devices, GPIO polarity, gate-drive behavior, and protection response are verified.

# 20. Native shell reference

The native simulator has the full engineering shell.

## 20.1 Layer tests

```text
bms test transport
bms test driver
bms test monitor
bms test topology
bms test measurement
bms test runtime
bms test balancing
bms test estimator
bms test persistence
bms test communications
bms test system
bms test all
```

## 20.2 Snapshot and runtime

```text
bms snapshot
bms measurement status
bms measurement refresh
bms measurement start
bms measurement stop
bms measurement period <ms>

bms runtime status
bms runtime process
bms runtime start
bms runtime stop

bms protection status
bms protection config
bms state
```

## 20.3 Balancing and estimator

```text
bms balancing status
bms balancing config
bms balancing enable
bms balancing disable

bms estimator status
bms estimator config
bms estimator set_soc <0..100>
bms estimator set_full_capacity <mAh>
bms estimator reset_counters
```

## 20.4 Configuration/persistence

```text
bms config show
bms config status
bms config defaults
bms config apply
bms config save
bms config load
bms config simulate_reboot
bms config erase

bms config set periods <measurement_ms> <runtime_ms>
bms config set uv <trip_mV> <release_mV>
bms config set ov <trip_mV> <release_mV>
bms config set charge_oc <trip_mA> <release_mA>
bms config set discharge_oc <trip_mA> <release_mA>
bms config set capacity <design_mAh> <initial_full_mAh>
```

## 20.5 Communications

```text
bms comm status
bms comm can publish
bms comm can dump

bms comm rs485 request status
bms comm rs485 request cells
bms comm rs485 request config
bms comm rs485 stage_uv <trip_mV> <release_mV>
bms comm rs485 apply
bms comm rs485 save
bms comm rs485 dump
```

## 20.6 System supervisor

```text
bms system status
bms system selftest
bms system check
bms system recover
bms system watchdog status
bms system watchdog inject_error <errno>
```

## 20.7 Diagnostic virtual controls

```text
bms raw <0|1>
bms inject cell <bq> <cell> <mV>
bms inject current <bq> <mA>
bms inject uv <bq> on|off
bms balance <global_mask>
bms switch release|all_off|charge_off|discharge_off
bms events show
bms events clear
bms scenario all
```

Manual `balance` and `switch` commands are diagnostic paths and bypass the normal automatic policy/authority flow. They are not the intended production control interface.

# 21. Validation evidence in the project

The supplied `VALIDATION.txt` records hardware-independent host tests that passed with strict warning flags:

```text
PASS: protection policy host tests
PASS: balancing policy host tests
PASS: estimator host tests
PASS: config + persistence store host tests
PASS: estimator persisted-state host tests
PASS: CAN + RS485 protocol host tests
PASS: system health policy host tests
```

The native virtual full-stack tests were also used during development to validate the integrated transport contract, semantic driver, monitor adapter, topology, measurement publication, protection/state transitions, balancing, estimation, configuration, communications, and system supervisor.

However, the final consolidated STM32 Zephyr builds still need to be executed in the user's actual NCS/Zephyr environment. A successful host test is not a substitute for target compilation or electrical validation.

# 22. Known gaps and extension points

This section is intentionally explicit so another engineer or AI does not silently assume missing functionality already exists.

## 22.1 Temperature

The BQ register map includes temperature commands and the legacy hardware uses BQ temperature inputs, but the current generic `bms_monitor` capability set, `bms_snapshot`, protection policy, and telemetry view do **not** yet carry temperature channels. Hardware Safety Status B/C are read by the BQ driver, but the generic fault mapping currently maps Safety Status A and PF-related flags only.

Temperature support should be added as a new generic monitor capability and snapshot data model rather than added as BQ-specific calls in protection or CAN.

## 22.2 Variable cell count

The architecture supports topology segment cell counts, but the provided 16S and 32S product initializers currently instantiate 16 cells per BQ. A 19S/20S implementation needs an explicit product topology and matching BQ cell configuration.

## 22.3 Automatic SOH learning

The plumbing exists, but no learned-capacity algorithm is implemented. Do not label the current capacity ratio as an automatically learned SOH.

## 22.4 Product inverter protocols

The current CAN profile is a reference protocol. A specific inverter profile should be another encoder/adapter that consumes `bms_telemetry_view`; it should not read hardware directly.

## 22.5 BQ Data Memory configuration

The driver contains register/Data Memory addresses, but the final STM32 runtime does not yet reproduce the complete legacy BQ startup programming/calibration sequence. Final product BQ Data Memory values must be defined, verified, and programmed separately.

## 22.6 Persistence

Real NVS support exists but is off by default. Approve flash sectors, bootloader strategy, image size, erase behavior, and endurance before enabling it.

## 22.7 32S electrical implementation

The 32S upper-AFE GPIO pins are a design specification for a future PCB. Isolation direction, power, creepage/clearance, fast protection interlocks, and high-side FET circuitry require schematic/layout design and validation.

# 23. Rules for future ports, including ESP32

The planned ESP32 port should preserve the same layer boundary:

```text
UNCHANGED
--------
BQ semantic driver contract
bms_monitor
bms_topology
bms_snapshot
measurement
protection
state machine
balancing
estimator
configuration format
CAN/RS485 protocol encoders
system policy

REPLACED / ADAPTED
------------------
Devicetree / board hardware description
I2C transport backend
CAN/TWAI backend
UART/RS485 backend
flash persistence backend
watchdog backend
platform startup/power details
```

If a future port requires edits deep inside protection/topology merely because the MCU changed, that is a sign the platform boundary has been broken.

# 24. DeepSeek / external-AI handoff instructions

When giving this project to DeepSeek or another code model, provide the full project zip plus `DEEPSEEK_PROJECT_CONTEXT.md`. The model should be instructed to use the source tree as authority and preserve these rules:

1. Do not merge BQ-specific register operations into protection, SOC, CAN, or application code.
2. Do not let protocols directly read the BQ; they consume the published snapshot/telemetry view.
3. Do not sum current from stacked AFEs; BQ0 is the explicit 32S current provider.
4. Do not use one AFE's PACK reading as the 32S pack voltage; current 32S topology sums both stack voltages.
5. Do not assume the 32S upper GPIO pins belong to an existing PCB; PE12/PB0/PB7 are reference design assignments.
6. Do not enable STM32 physical actuation by default.
7. Do not describe default protection/balancing values as production-qualified thresholds.
8. Do not claim automatic SOH learning exists.
9. Do not claim the reference CAN profile is Pylontech/Victron/J1939/CANopen compatible.
10. Do not silently assume temperature support exists in the generic snapshot/policy layer.
11. Do not enable NVS until the flash map is approved.
12. For a new MCU such as ESP32, replace the platform backends and board description first; keep the generic BMS core unchanged wherever possible.

# Appendix A. Default application configuration

| Parameter | Default value | Status |
|---|---:|---|
| Measurement period | 200 ms | Development default |
| Runtime period | 100 ms | Development default |
| UV trip | 2800 mV/cell | Development default |
| UV release | 3000 mV/cell | Development default |
| OV trip | 3650 mV/cell | Development default |
| OV release | 3500 mV/cell | Development default |
| Charge OC trip | +30000 mA | Development default |
| Charge OC release | +25000 mA | Development default |
| Discharge OC trip magnitude | 50000 mA | Development default |
| Discharge OC release magnitude | 40000 mA | Development default |
| Snapshot stale limit | 1000 ms | Development default |
| Balancing enabled | Yes | Development default |
| Balancing start voltage | 3400 mV | Development default |
| Balancing start delta | 20 mV | Development default |
| Balancing stop delta | 10 mV | Development default |
| Balancing max current magnitude | 5000 mA | Development default |
| Max balancing cells | 4 | Development default |
| Design capacity | 100000 mAh | Simulator/default model |
| Initial full capacity | 100000 mAh | Simulator/default model |
| Initial SOC | 50.00% | Simulator/default model |
| Charge efficiency | 100.0% | Simulator/default model |
| Max estimator integration gap | 1000 ms | Development default |

# Appendix B. Generic fault flags

The current generic monitor fault bitmap contains:

```text
bit 0  CELL_UNDERVOLTAGE
bit 1  CELL_OVERVOLTAGE
bit 2  OVERCURRENT_CHARGE
bit 3  OVERCURRENT_DISCHARGE
bit 4  SHORT_CIRCUIT_DISCHARGE
bit 24 PERMANENT_FAIL
```

Protection translates these into the separate `HW_*` reason namespace before making charge/discharge decisions.

# Appendix C. First production-hardware checklist

Before enabling hardware actuation on STM32:

- Confirm both STM32 builds compile in the actual NCS/Zephyr workspace.
- Confirm the MCU clock tree and UART console operate correctly.
- Confirm BQ0 responds to `bms bq_ping 0`.
- For 32S, confirm isolated BQ1 responds to `bms bq_ping 1`.
- Confirm I2C pull-ups and waveform quality on both buses.
- Confirm ISO1640 upper-side supply and floating-ground implementation.
- Confirm ALERT polarity and interrupts on every monitor.
- Confirm `RST_SHUT` polarity and timing.
- Confirm DFETOFF/BOTHOFF configuration before relying on the pin.
- Program and verify BQ Data Memory for the exact cell chemistry and pack.
- Calibrate cell/stack/PACK voltage scaling.
- Calibrate shunt/current scaling and current sign.
- Verify hardware UV/OV/OCC/OCD/SCD response with controlled sources.
- Verify FET OFF commands first with no high-energy load.
- Verify balancing one cell at a time and measure balance current/temperature.
- Verify CAN transceiver, bit rate, termination, and external protocol behavior.
- Verify RS485 DE/RE timing, termination, and framing.
- Approve NVS flash partition before enabling persistent flash writes.
- Verify watchdog reset behavior on real hardware.
- Run thermal, fault-injection, pack-level, EMC, and safety validation before production.

# Appendix D. Project identity summary

```text
Project:           STARZ BMS Zephyr v1.0
Primary MCU:       STM32F446ZE
AFE:               TI BQ76952
16S profile:       1 x BQ76952
32S profile:       2 x stacked BQ76952
32S top I2C:       I2C2 through ISO1640 reference design
RTOS/framework:    Zephyr
Simulation:        Zephyr native_sim
Core principle:    hardware-neutral BMS domain above bms_monitor
Current sign:      +charge / -discharge
Max logical cells: 32
Physical actuation disabled by default on STM32
```

