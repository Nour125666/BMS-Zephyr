# Real STM32 hardware mapping

Yes: in Zephyr, the CubeMX-like hardware mapping is normally done with
**Devicetree + STM32 pinctrl**, not CubeMX-generated HAL init code.

Conceptually:

```text
SoC DTS knows STM32 peripherals
        ↓
board DTS/overlay enables I2C1
        ↓
pinctrl selects the physical SCL/SDA pins + alternate function
        ↓
BQ child node gives the 7-bit I2C address
        ↓
GPIO properties describe ALERT / RST_SHUT / DFETOFF pins
        ↓
Zephyr STM32 drivers configure clocks, GPIO AF and I2C
```

Illustrative only:

```dts
&i2c1 {
    status = "okay";
    pinctrl-0 = <&i2c1_default>;
    pinctrl-names = "default";

    bq0: bq76952@8 {
        compatible = "ti,bq76952";
        reg = <0x08>;

        alert-gpios = <&gpioc 5 GPIO_ACTIVE_HIGH>;
        rst-shut-gpios = <&gpiob 14 GPIO_ACTIVE_HIGH>;
        dfetoff-gpios = <&gpiob 6 GPIO_ACTIVE_HIGH>;
    };
};
```

STM32 pinctrl (example only; actual pins/AF depend on the final MCU/package):

```dts
&pinctrl {
    i2c1_default: i2c1_default {
        group1 {
            pinmux = <STM32_PINMUX('B', 8, AF4)>,
                     <STM32_PINMUX('B', 9, AF4)>;
            drive-open-drain;
        };
    };
};
```

The real driver obtains typed descriptions using Zephyr helpers such as
`I2C_DT_SPEC_GET(...)` and `GPIO_DT_SPEC_GET(...)`.

Do not copy the example pin numbers blindly. The exact pinctrl must come from
the final PCB schematic and exact STM32 part/package.
