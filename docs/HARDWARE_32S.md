# STM32F446 maximum-32S hardware profile

This is a NEW STARZ reference hardware definition derived from the proven 16S MCU mapping and TI stacked-BQ769x2 design guidance.

## Electrical architecture

```text
Cells 1..16  -> BQ0 / bottom -> I2C1 PB8/PB9 -> STM32F446
Cells 17..32 -> BQ1 / top    -> ISO1640 -> I2C2 PF1/PF0 -> STM32F446
```

The top monitor floats at the upper 16-cell group potential, so its digital interface must cross an isolation/level-shifting boundary.

## Deliberate STARZ choice: two MCU I2C controllers

TI TIDA-010247 demonstrates a shared I2C bus through ISO1640 and changes one BQ address using OTP because both devices default to the same address.

This STARZ reference instead uses:

- bottom BQ on I2C1 @ 0x08
- top BQ on I2C2 @ 0x08 through ISO1640

Therefore no BQ OTP address change is required. STM32F446 already exposes both buses in the legacy CubeMX design.

## Provider roles used by software

- cells 1..16: BQ0
- cells 17..32: BQ1
- pack current provider: BQ0 (bottom monitor)
- FET/switch provider: BQ1 (top monitor/high-side FET architecture)
- pack voltage: sum of both local stack voltages
- faults: OR/aggregate both monitors
- balancing: topology splits the global 32-bit mask into two local 16-bit masks

## Proposed upper-monitor isolated GPIO routing

The new 32S PCB should route these through an appropriate isolated/level-shifted GPIO channel (for example a direction-compatible digital isolator):

- PE12 <- TOP_ALERT
- PB0  -> TOP_RST_SHUT
- PB7  -> TOP_DFETOFF

These pins were unused/unlabelled in the legacy 16S MCU project. Because the 32S PCB does not yet exist, these are design assignments, not claims about an existing board.

## Important hardware requirements not expressible in Devicetree

- ISO1640 secondary-side power must be available whenever top-BQ communication is required.
- Top BQ and isolator supply strategy must respect the floating upper-cell-group ground.
- Fast bottom-AFE protection-to-top-FET hardware paths should be implemented electrically if reproducing TI's fastest SCD/OCD shutdown concept; host software is not a substitute for that path.
- creepage/clearance, transceiver isolation and high-voltage layout are PCB design responsibilities.
