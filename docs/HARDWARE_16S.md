# STM32F446 ≤16S hardware profile

This profile reconstructs the legacy CubeMX hardware contract around STM32F446ZET6.

- BQ76952: I2C1 PB8/PB9, 100 kHz, 7-bit address 0x08
- BQ ALERT: PC5 input/interrupt
- BQ RST_SHUT: PB14 output, inactive low
- BQ DFETOFF: PB6 output, inactive low until the product profile enables the function
- CAN1: PD0 RX / PD1 TX, 500 kbit/s
- RS485: UART4 PC10/PC11 at 115200 + PB2 DE/RE
- ESP32 bridge: UART5 PE8/PE7 at 115200
- debug/console: USART3 PD8/PD9 at 115200
- LCD: I2C3 PA8/PC9
- HSE 8 MHz, SYSCLK 168 MHz

The legacy CubeMX project configured PC5/ALERT and PF11/START inconsistently as outputs. The product hardware map documents both as inputs; this Zephyr profile treats them as inputs.
