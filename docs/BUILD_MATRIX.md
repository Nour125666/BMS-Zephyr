# Build matrix

## Full virtual 32S simulator

```bash
west build -p always -b native_sim/native/64 . -- -DCONF_FILE=config/native_sim.conf
```

Run the returned `zephyr.exe`, connect to its PTY, then use:

```text
bms test all
bms scenario all
```

## STM32F446 maximum 16S

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_16s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_16s.overlay
```

## STM32F446 maximum 32S

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_32s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_32s.overlay
```

The Nucleo board is used as Zephyr's STM32F446ZE SoC/base definition; the overlays replace the application pin/peripheral mapping with the STARZ hardware contract.

## Optional internal-flash NVS

An NVS backend and reference storage overlay are included but disabled by default because the final application/linker/bootloader memory map must be approved first.

## First hardware bring-up rule

Both STM32 profiles default to `CONFIG_BMS_HW_ACTUATION_ENABLE=n`. Keep it that way until the BQ and gate-drive behavior are verified on the physical board.
