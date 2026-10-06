# STARZ BMS Zephyr v1.0 — final integrated project

This folder is the consolidated project produced from the BQ76952 driver, generic monitor abstraction, topology/snapshot model, measurement service, protection/state machine, balancing, SOC/energy estimator, configuration/persistence, CAN/RS485 communications, system supervisor and the STM32F446 hardware definitions.

It supports three build profiles from one source tree:

```text
native_sim 32S virtual model
STM32F446 maximum 16S / 1x BQ76952
STM32F446 maximum 32S / 2x stacked BQ76952
```

## Architecture

```text
                    application protocols / shell
                             |
                    system supervisor
                             |
       +---------------------+--------------------+
       |                     |                    |
   protection            balancing            estimator
       |                     |                    |
       +---------------------+--------------------+
                             |
                    published snapshot
                             |
                       measurement
                             |
                         topology
                 +-----------+-----------+
                 |                       |
             monitor 0                monitor 1
                 |                       |
          BQ76952 adapter          BQ76952 adapter
                 |                       |
           semantic driver          semantic driver
                 |                       |
        transport backend        transport backend
                 |                       |
       virtual model or Zephyr I2C / STM32 hardware
```

## Hardware profiles

### <=16S

One BQ76952 on I2C1 PB8/PB9.

### >16S up to 32S

Two BQ76952 devices:

```text
BQ0 bottom -> I2C1 direct
BQ1 top    -> I2C2 through ISO1640 isolation
```

Both remain at I2C address `0x08` because they are on different STM32 I2C controllers. This avoids irreversible OTP address programming.

See `docs/HARDWARE_16S.md`, `docs/HARDWARE_32S.md`, and `docs/BUILD_MATRIX.md`.

## Important validation boundary

The native simulator validates software behavior. STM32 builds validate Devicetree/pinctrl/API integration. Actual electrical behavior still requires the physical PCB/BQ/battery hardware: I2C signal integrity, isolation power, ALERT polarity/timing, FET switching, shunt/current calibration, CAN/RS485 transceivers, flash endurance, watchdog reset and pack-level safety testing.

## Physical-actuation safety default

The STM32 build profiles ship with `CONFIG_BMS_HW_ACTUATION_ENABLE=n`. This is deliberate: the complete decision logic runs, but normal runtime FET/balancing writes are inhibited during first board bring-up. Enable physical actuation only after I2C, BQ configuration, FET polarity and protection paths are verified.

## Current persistence choice

The STM32 profiles default to the RAM persistence backend so the project does not silently reserve half of internal flash without an approved linker/bootloader plan. A real NVS backend and optional reference partition overlay are included under `platform/stm32/` and `overlays/stm32f446_storage_nvs.overlay`.

## Next documentation step

The code is now organized so a separate user manual/datasheet can describe each layer, data flow, commands, build profile and hardware contract without changing the implementation.
