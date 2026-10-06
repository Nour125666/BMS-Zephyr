# Source basis and design status

## Legacy-derived / confirmed for the 16S profile

- STM32F446ZET6 / LQFP144
- I2C1 PB8/PB9 for BQ76952
- I2C2 PF1/PF0 available
- I2C3 PA8/PC9 LCD
- CAN1 PD0/PD1
- UART4 PC10/PC11 + PB2 RS485 DE/RE
- UART5 PE8/PE7 ESP32 bridge
- USART3 PD8/PD9 console/GUI
- PC5 ALERT, PB14 RST_SHUT, PB6 DFETOFF
- 8 MHz HSE and 168 MHz CubeMX clock tree

## TI-guidance-derived for stacked 32S

- two BQ769x2 devices, 16 cells each
- bottom BQ shares MCU/pack-negative ground
- upper BQ requires isolation/level shifting to the MCU
- ISO1640 is a documented isolated-I2C option
- bottom BQ monitors pack current
- top BQ drives the high-side charge/discharge MOSFET stage in the documented stacked architecture

## STARZ-created 32S design choices

- use I2C1 for BQ0 and I2C2+ISO1640 for BQ1
- keep both BQs at default 0x08 because the buses are separate
- route isolated upper ALERT/RST_SHUT/DFETOFF to PE12/PB0/PB7

These 32S pin assignments are a design specification for a future PCB and require schematic/layout implementation and hardware validation.
