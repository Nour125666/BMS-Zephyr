---
title: "The STARZ BMS Zephyr v1.0 Book"
subtitle: "A from-first-principles explanation of the battery-management firmware project, written to be understood and re-explained"
author: "Companion volume to the STARZ BMS Zephyr v1.0 Technical Manual"
date: "06 October 2026"
lang: en-US
---

> **What this book is.** The project already ships a *Technical Manual* — a precise
> reference written for engineers who inspect, port, and extend the code. This book
> is different. It is a **teaching book**. It assumes you did not write the firmware
> and are not yet an embedded expert. It builds every idea from the ground up,
> explains *why* each design decision was made, walks the data from the battery
> terminals to the CAN bus, and finishes with a section you can use to **explain the
> project out loud** — in an interview, a portfolio review, or a design discussion.
>
> The source code is always the final authority. Where this book and the code
> disagree, believe the code. Every claim here is traceable to a file and line
> number so you can check it yourself.

> **Safety boundary (read this once, believe it always).** This project contains
> real battery-protection logic, but the STM32 build profiles deliberately ship with
> physical FET and balancing actuation **disabled**
> (`CONFIG_BMS_HW_ACTUATION_ENABLE=n`). Nothing here has been validated on a real
> 16S or 32S board. Thresholds, GPIO assumptions, current calibration, isolation
> routing, and FET behaviour are **development values**, not product-qualified
> values. Never enable actuation on hardware until the bring-up checklist in
> Chapter 22 is complete.

---

# How to read this book

If you have 20 minutes, read Chapters 1–4 and Chapter 24 (the interview cheat-sheet).

If you want to understand the whole thing, read it in order. The chapters are
written so that each one only depends on the ones before it.

Part I — **Foundations** (Ch. 1–5): batteries, BMS, the BQ76952 chip, Zephyr RTOS,
and the tools. No prior embedded knowledge assumed.

Part II — **Architecture** (Ch. 6–8): the one big idea of the project (layering),
the source tree, and the data-flow picture.

Part III — **The layers, one at a time** (Ch. 9–18): transport, driver, monitor,
topology, snapshot, measurement, protection, state machine, balancing, estimator,
configuration, persistence, protocols, communications, supervisor.

Part IV — **Hardware and builds** (Ch. 19–21): the 16S and 32S hardware profiles and
how the three builds are produced.

Part V — **Running, testing, and extending** (Ch. 22–26): the simulator, the shell,
bring-up, known gaps, and the review of mistakes and suggested improvements.

Part VI — **Reference** (Appendices): glossary, unit tables, command reference.

---

# PART I — FOUNDATIONS

# 1. What a battery-management system is, and why it exists

## 1.1 The raw problem

A lithium-ion battery cell stores energy chemically. Its useful voltage is roughly
**3.0 V (empty) to 4.2 V (full)**. Push it below about 2.5–2.8 V and you permanently
damage it (or plate metal inside it). Push it above about 4.2–4.3 V and you risk
venting, fire, or explosion. Push too much current through it and it overheats.

One cell is only ~3.7 V, so real packs wire many cells **in series**. The STARZ
project targets two sizes:

* **16S**: sixteen cells in series → nominal ≈ 51 V, full ≈ 67 V.
* **32S**: thirty-two cells in series → nominal ≈ 102 V, full ≈ 134 V.

When cells are in series, the same current flows through all of them, but they are
not identical. Some are a little stronger, some a little weaker. Over hundreds of
cycles the weakest cell drifts. The pack is only as healthy as its weakest cell.

## 1.2 The job of a BMS

A **Battery Management System (BMS)** is the electronics and firmware that keeps the
pack safe and reports on it. It does four things:

1. **Measure** — every cell voltage, the pack current, the pack voltage, and
   (ideally) temperature.
2. **Protect** — disconnect charging if any cell is too high; disconnect discharging
   if any cell is too low, if current is too high, or if there is a short circuit.
3. **Balance** — deliberately bleed a little charge out of the *highest* cells so all
   cells converge to the same voltage over time.
4. **Communicate** — tell the outside world the state of charge, the faults, and the
   measurements, usually over CAN or RS485.

## 1.3 The fundamental problem: "you can't just use software"

A transistor that switches 100 A at 100 V must react in **microseconds** to a short
circuit, far faster than a general-purpose firmware loop can be trusted to act every
single time. So a real BMS is always a **partnership** between:

* a dedicated analog chip next to the cells that can trip in hardware
  (in this project, the **TI BQ76952**, called the "AFE" — Analog Front End), and
* firmware that supervises it, reports it, configures it, and adds slower
  software-level policies on top.

This project is the *firmware* half, plus the abstraction layers that let that
firmware look the same whether the AFE is real hardware or a software model.

## 1.4 Vocabulary you will meet

| Term | Meaning |
|---|---|
| **AFE** | Analog Front End. The chip measuring cells and driving the protection FETs. Here the BQ76952. |
| **Cell** | One battery cell, ~3.0–4.2 V. |
| **Pack** | The whole series string (16 or 32 cells). |
| **Stack** | The voltage across the monitored cells of one AFE (the BQ "stack" voltage). |
| **FET / MOSFET** | The electronic switch that connects/disconnects the pack to charger/load. |
| **CHG FET** | The switch that allows charging. |
| **DSG FET** | The switch that allows discharging. |
| **Shunt** | A tiny, precise resistor in the current path; the AFE measures its voltage to learn current. |
| **SOC** | State of Charge — "how full", 0–100 %. |
| **SOH** | State of Health — "how much capacity is left", 0–100 %. |
| **Coulomb counting** | Tracking charge by integrating current over time (`charge += current × time`). |
| **RTOS** | Real-Time Operating System — a small scheduler for firmware tasks. Here, Zephyr. |
| **Devicetree** | A data file describing *which hardware is wired to which pin*. |
| **Overlay** | A Devicetree file that adds board-specific details on top of a base board. |

---

# 2. The BQ76952 in plain language

The BQ76952 (Texas Instruments) is a single chip that can sit next to a stack of
**3 to 16 series cells** and do the high-speed analog work:

* Measure each cell voltage (16 channels).
* Measure the pack current through a shunt.
* Measure stack/PACK/LD voltages.
* Compare each voltage against **Data Memory** thresholds that you program, and
  autonomously trip the FETs when a threshold is crossed.
* Drive several external MOSFETs (CHG, DSG, pre-charge, pre-discharge, and a
  body-diode / DFETOFF function).
* Balance cells by enabling internal bleed resistors.
* Raise an **ALERT** pin when something happens.

Because it only handles **16 cells per chip**, a 32S pack needs **two** of them,
stacked. That single fact drives a large part of this project's design.

## 2.1 How the MCU talks to it

The BQ76952 speaks **I2C**. There are two kinds of conversation:

1. **Direct commands** — one-byte command addresses (for example `0x14` = Cell 1
   voltage). Simple but limited.
2. **Subcommands and Data Memory** — a two-step "mailbox" protocol using a
   **transfer buffer**:
   * Write the 16-bit subcommand/address into command register `0x3E`.
   * The chip puts its answer into a buffer at `0x40..0x5F`.
   * Register `0x61` says how long the answer is; register `0x60` holds a checksum.

This project implements both, and wraps them so that the rest of the firmware never
sees a raw register number (Chapter 9).

## 2.2 The BQ76952 "safety" flags

The chip has several status registers. The two important ones for this project:

* **Safety Alert A (`0x02`)** — "this is happening *right now*".
* **Safety Status A (`0x03`)** — "this already tripped and is latched/delayed".

Their bit assignment (verified against the TI BQ76952 Technical Reference Manual) is:

| Bit | Safety Status A (0x03) | Meaning |
|---|---|---|
| 7 | SCD | Short Circuit in Discharge |
| 6 | OCD2 | Overcurrent in Discharge, level 2 |
| 5 | OCD1 | Overcurrent in Discharge, level 1 |
| 4 | OCC | Overcurrent in Charge |
| 3 | COV | Cell Overvoltage |
| 2 | CUV | Cell Undervoltage |

The driver reads both registers and several others, and maps the important bits into
a **generic fault bitmap** (Chapter 9). Note that the previous technical manual says
Safety Status B and C (temperature, etc.) are read but not yet mapped — see the
Known Gaps chapter.

## 2.3 Default I2C address and the 32S trick

Every BQ76952 powers up at 7-bit I2C address **0x08**. If you put two of them on the
same I2C bus, they collide. TI's reference design solves this by permanently
reprogramming one chip's address (OTP — one-time programmable, irreversible). This
project deliberately avoids OTP:

> **Keep both chips at 0x08, but put them on two different I2C buses.**

The STM32F446 has I2C1 and I2C2, so bottom AFE → I2C1, top AFE → I2C2. No chip is
irreversibly modified. This is one of the project's nice design decisions, and a
great thing to mention in an interview.

---

# 3. Zephyr RTOS in plain language

Zephyr is a **real-time operating system** for microcontrollers. You can think of it
as "a tiny Linux-like environment for a chip with 128 KB of RAM". It provides a
scheduler, threads, timers, drivers, a console shell, and — crucially — a way to
describe hardware in data files instead of code.

## 3.1 The Zephyr concepts this project actually uses

| Concept | What it is | Where you see it here |
|---|---|---|
| **Thread** | A function that runs "in parallel" via the scheduler. | The RS485 receive thread in `main_stm32.c`. |
| **`k_work_delayable`** | A deferred work item that runs on a system work queue, and can reschedule itself. Used instead of dedicated threads for periodic jobs. | `bms_measurement.c`, `bms_core_runtime.c`, `bms_communications.c`, `bms_system.c`. |
| **`k_mutex`** | A lock so two pieces of code do not corrupt shared data. | Every service has one — `struct bms_measurement.lock`, etc. |
| **`atomic_t`** | Lock-free flags/values. | `running` flags; the alert counters. |
| **`k_uptime_get()`** | Milliseconds since boot. The project's only clock for age/staleness. | Snapshot age, estimator integration. |
| **Devicetree (DTS)** | Data files describing the board: which pins, which buses, which devices. | `overlays/*.overlay`, `dts/bindings/**`. |
| **Overlay** | A DTS file layered onto a base board. | `overlays/stm32f446_16s.overlay`, `..._32s.overlay`. |
| **Kconfig** | A menu of compile-time options. | Root `Kconfig`, `prj.conf`, `config/*.conf`. |
| **Shell** | An interactive command console over a UART. | `bms ...` commands in both `main_native.c` and `main_stm32.c`. |
| **`native_sim`** | A Zephyr "board" that builds your firmware as a normal PC program, so you can run and test it without hardware. | `-b native_sim/native/64`. |

## 3.2 The three ideas that matter most

**(a) Hardware is described, not hard-coded.** In a CubeMX/HAL project you would
write `HAL_GPIO_Init(...)` and `HAL_I2C_Init(...)`. In Zephyr you write DTS text that
says "I2C1 lives on PB8/PB9 at 100 kHz, and there is a BQ76952 at address 0x08 on
it". Zephyr's drivers then configure the hardware. You will see this in Chapter 19.

**(b) Work queues instead of busy loops.** It is tempting to write
`while (1) { read(); sleep(200); }`. Zephyr lets you schedule a `k_work_delayable`
that automatically reschedules itself, so the CPU sleeps and the scheduler stays
responsive. Every periodic service here does exactly this.

**(c) The same code runs on hardware and on a PC.** `native_sim` compiles the same
C into a Linux/Windows-style executable. Because the project put the hardware behind
a small interface, the *entire* BMS logic runs in simulation. That is what makes the
project testable and is the core of the portfolio story.

---

# 4. The toolchain: how this project is built and run

You do **not** need hardware to build and run the main simulator. You need a Zephyr
workspace (the standard `west` tool plus a Zephyr SDK).

## 4.1 One-time setup (conceptual)

1. Install the Zephyr SDK and `west`.
2. Create a Zephyr workspace and fetch Zephyr.
3. Put this project somewhere the workspace can see it (or build it standalone with
   `ZEPHYR_BASE` set).
4. Open a terminal where `west` works.

## 4.2 The three builds

From the project root:

```bash
# 1) Full virtual 32S simulator (no hardware needed)
west build -p always -b native_sim/native/64 . -- -DCONF_FILE=config/native_sim.conf

# 2) STM32F446, up to 16S, one BQ76952
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_16s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_16s.overlay

# 3) STM32F446, up to 32S, two stacked BQ76952
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_32s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_32s.overlay
```

The `nucleo_f446ze` board is used only as **"the STM32F446ZE SoC definition"**.
The overlays replace the Nucleo's pin/peripheral mapping with the STARZ hardware
contract. This is why the project does not need a custom board folder.

## 4.3 Running the simulator

After building the native simulator, run the produced executable (on Linux/macOS it
is usually `build/zephyr/zephyr.exe`; on Windows it is also `zephyr.exe`). It prints
a pseudo-terminal (PTY) path. Connect a serial terminal to that PTY and you get the
`bms` shell.

Then try:

```
bms test all
bms scenario all
bms snapshot
bms system status
```

`bms test all` runs a layer-by-layer smoke test. `bms scenario all` runs a
behaviour-level regression suite (normal, undervoltage, overvoltage, overcurrent,
hardware UV, balancing, communications). If both print `[PASS]`, the software stack
is behaving as designed.

---

# 5. A worked example so the math is concrete

Before the architecture, let us make SOC and coulomb counting concrete, because the
estimator is one of the scariest-looking modules and is actually simple.

Suppose the pack design capacity is **100 000 mAh** (100 Ah) and it starts at
**50 %**. That means the estimator starts believing there are 50 000 mAh = **50 000
000 µAh** of charge in the pack. Internally, charge is tracked in **µAh**
(microamp-hours) so small currents over short times are not lost to rounding.

Now a current of **−20 000 mA** (discharge) flows for **1 second** (1000 ms). Charge
removed:

```
charge_removed[uAh] = |I[mA]| × t[ms] ÷ 3600
                    = 20000 × 1000 ÷ 3600
                    = 5555.5 µAh
```

(Cut a 20 A load for one second and you have moved 5.56 mAh — check: 20 A × 1 s =
20 As = 20/3600 Ah = 5.56 mAh. The formula agrees.)

The new remaining charge drops by 5555 µAh, and SOC is recomputed as
`remaining ÷ full_capacity × 100 %`. Because the current was negative, the estimator
also adds 5555 µAh to the **discharged** counter, and because current was non-zero,
adds it to **throughput**.

Two subtleties the real code handles, which explain why it looks more complicated
than the formula above:

* **Don't integrate twice.** The core runtime ticks every 100 ms but measurement
  publishes every 200 ms, so the estimator sees the *same* snapshot twice. It stores
  the last sequence number and ignores duplicates (`bms_estimator.c:153`).
* **Don't integrate across a gap.** If the time step is zero, negative, or longer
  than the configured limit (default 1000 ms), the estimator refuses to guess what
  happened, counts a "skipped interval", and re-anchors on the next snapshot
  (`bms_estimator.c:183`).

That is the entire idea. The scary-looking fixed-point code in `bms_estimator.c`
exists only to keep fractional remainders so tiny currents still add up correctly.

---

# PART II — ARCHITECTURE

# 6. The one big idea: a hard layer boundary

If you remember one sentence from this book, remember this one:

> **Below `bms_monitor` is hardware/vendor-specific. Above `bms_monitor` is
> battery-domain logic, and it must be hardware-neutral.**

Everything else follows from it. The project exists in layers, and the rule is that
**information flows up, control flows down**, and no layer may "reach through"
another.

```text
                     Application / shell / external protocols
                                      |
                             System supervisor
                                      |
              +----------------------+----------------------+
              |                      |                      |
          Protection             Balancing              Estimator
              |                      |                      |
              +----------------------+----------------------+
                                      |
                            Published snapshot (one coherent picture)
                                      |
                                 Measurement
                                      |
                                   Topology
                          +-----------+-----------+
                          |                       |
                      Monitor 0               Monitor 1        <- generic API
                          |                       |
                    BQ76952 adapter         BQ76952 adapter
                          |                       |
                    semantic driver         semantic driver
                          |                       |
                    transport backend       transport backend
                          |                       |
             virtual model  OR  Zephyr I2C / STM32 hardware
```

Why this matters:

* To **port** to a new MCU (the project plans ESP32), you replace the bottom block
  (transport, platform backends, board description) and keep everything above
  unchanged.
* To **test** without hardware, you replace only the very bottom with a virtual
  model, and every layer above runs unchanged. That is what `native_sim` does.
* To **reason about safety**, you can point at exactly one place where decisions are
  made (protection) and one place where they are published (measurement).

## 6.1 The two interfaces that carry the whole design

**`bms_monitor`** (in `include/bms/bms_monitor.h`) is the hardware-neutral view of
one AFE. It is a struct of function pointers (`ops`) plus a capability bitmap. Any
chip that can measure cells can implement it. It offers:

* `read_sample`, `read_faults`, `read_switch_state`
* `set_switch`, `get_balance_mask`, `set_balance_mask`

and advertises capabilities such as `BMS_MON_CAP_CELL_VOLTAGE`,
`BMS_MON_CAP_CURRENT`, `BMS_MON_CAP_BALANCING`.

**`bms_snapshot`** (in `include/bms/bms_snapshot.h`) is the hardware-neutral view of
the *whole pack*: all cell voltages, current, pack voltage, min/max, faults, balance
mask, switch state, a sequence number, and a timestamp.

Everything above the snapshot only ever reads snapshots. Nothing above the monitor
ever touches I2C or a BQ register. That is the rule, and the rest of the book shows
it being obeyed.

---

# 7. The source tree, annotated

```text
starz-bms-zephyr-v1.0-final-documented/
|
|-- CMakeLists.txt          Build: which sources compile for which platform.
|-- Kconfig                 Compile-time menu: platform + pack profile + safety switches.
|-- prj.conf                Default config (native sim).
|
|-- config/                 Per-profile Kconfig fragments.
|   |-- native_sim.conf
|   |-- stm32_16s.conf
|   `-- stm32_32s.conf
|
|-- overlays/               Devicetree overlays (the "pin map" as data).
|   |-- stm32f446_16s.overlay
|   |-- stm32f446_32s.overlay
|   `-- stm32f446_storage_nvs.overlay   (optional flash partition)
|
|-- dts/bindings/bms/       YAML descriptions of the custom DTS nodes.
|   |-- ti,bq76952.yaml
|   `-- starz,bms-board-io.yaml
|
|-- include/bms/            PUBLIC interfaces (the contracts between layers).
|   |-- bms_types.h             basic enums/structs (fault flags, samples, switches)
|   |-- bms_monitor.h           generic AFE API
|   |-- bms_topology.h          mapping monitors -> one logical pack
|   |-- bms_snapshot.h          the coherent whole-pack picture
|   |-- bms_measurement.h       acquisition + publication
|   |-- bms_protection.h        protection decision
|   |-- bms_balancing.h         balancing decision
|   |-- bms_state_machine.h     operating state + switch authority
|   |-- bms_estimator.h         SOC/SOH/energy
|   |-- bms_config.h            the app config object
|   |-- bms_config_store.h      versioned persistent image
|   |-- bms_events.h            in-RAM event log
|   |-- bms_protocol.h          CAN + RS485 encoders/decoders
|   |-- bms_communications.h    telemetry service + backends
|   |-- bms_system.h            supervisor
|   |-- bms_system_policy.h     pure health-classification policy
|   `-- bms_watchdog.h          watchdog backend interface
|
|-- subsys/bms/             IMPLEMENTATION of all the generic services.
|-- drivers/bms_monitor/bq76952/   The BQ76952-specific code (driver + adapter).
|-- platform/stm32/         The STM32-specific backends (GPIO, CAN, RS485, WDT, NVS).
|-- tests/                  Host unit tests + the virtual BQ model.
|-- src/
|   |-- main_native.c           Simulator entry point + full engineering shell.
|   `-- main_stm32.c            Hardware entry point + small bring-up shell.
|-- reference/VS_BMS_16S_reference.ioc   The legacy CubeMX project (the origin).
`-- docs/                  Manuals, hardware notes, and this book.
```

The next chapter shows how a single measurement travels through every one of these
boxes.

---

# 8. One data point's journey (the mental model to keep)

Picture a single instant. The core runtime wants to know whether the battery is safe.
Here is the complete path, in order. Keep this diagram in your head; every later
chapter zooms into one arrow.

```text
STEP 1  BQ76952 hardware (or virtual model)
          stores cell voltages, current, fault bits

STEP 2  transport  (bq76952_transport_virtual.c  or  bq76952_transport_i2c.c)
          moves raw bytes over I2C / from the model

STEP 3  semantic driver  (bq76952.c)
          converts registers -> typed engineering values (mV, mA)
          and Safety Status A bits -> generic fault bitmap

STEP 4  BQ adapter  (bms_monitor_bq76952.c)
          wraps the driver in the generic bms_monitor API

STEP 5  topology  (bms_topology.c)
          reads every monitor, renumbers local cells to global cells,
          picks the current provider, sums stack voltages for 32S,
          ORs faults, builds one bms_snapshot

STEP 6  measurement  (bms_measurement.c)
          stamps the snapshot with a timestamp, publishes it under a mutex

STEP 7  core runtime  (bms_core_runtime.c)  -- every 100 ms
          copies the latest snapshot and feeds it to three consumers:
            -> protection  -> "what is allowed?"
            -> estimator   -> "how full is it?"
            -> balancing   -> "which cells should bleed?"
          then the state machine turns "allowed" into a switch command

STEP 8  actuation (only if enabled)
          state machine -> topology -> monitor -> driver -> BQ subcommands
          balancing policy -> topology -> monitor -> BQ balance mask

STEP 9  communications  -- every 500 ms
          collects snapshot + protection + state + estimator into
          one bms_telemetry_view, then encodes CAN and RS485 frames

STEP 10 system supervisor  -- every 250 ms
          checks that steps 6-9 are alive and fresh, latches SAFE on
          battery-control failure, feeds the watchdog
```

Two design consequences worth naming explicitly:

* **There is exactly one publisher of measurements (step 6).** Protection, CAN, and
  the estimator all read the *same* snapshot. This avoids a classic bug where
  protection acts on one cell reading while the display shows a later, different one.
* **Actuation is the only reverse path.** Decisions flow down only through the state
  machine (switches) and the balancing policy (balance mask), and only when actuation
  is enabled.

---

# PART III — THE LAYERS, ONE AT A TIME

# 9. Layer 1: transport, driver, and the BQ adapter

These are the three lowest layers, all inside `drivers/bms_monitor/bq76952/`. They
exist so that everything above sees only the generic `bms_monitor` API.

## 9.1 Transport — "how are bytes exchanged?"

The transport layer answers exactly one question: *how do bytes get to and from the
BQ76952?* It contains no battery logic. There are **two interchangeable
implementations** with the same function names:

| File | Used by | Talks to |
|---|---|---|
| `tests/virtual_bq/bq76952_transport_virtual.c` | `native_sim` | an in-RAM software model |
| `drivers/.../bq76952_transport_i2c.c` | STM32 | real Zephyr I2C |

Both implement the declarations in `bq76952_transport.h`:

```c
int bq76952_transport_init(struct bq76952_transport *, uint8_t instance_id);
int bq76952_direct_read  (...);   /* read direct command (e.g. a cell voltage) */
int bq76952_direct_write (...);
int bq76952_subcmd_command(...);  /* fire a command-only subcommand */
int bq76952_subcmd_read  (...);   /* subcommand with data reply */
int bq76952_subcmd_write (...);
int bq76952_dm_read      (...);   /* Data Memory read */
int bq76952_dm_write     (...);   /* Data Memory write */
```

`instance_id` is the key: **0 = bottom/single AFE, 1 = top AFE**. The real transport
turns that number into a Devicetree node (`bq0` or `bq1`) and gets an `i2c_dt_spec`
for it (`bq76952_transport_i2c.c:371`). That is the *only* place the number 0 or 1
means anything hardware-related.

### The real I2C transport's job (worth understanding once)

The real transport is more than a wrapper. It implements the BQ mailbox protocol
correctly, which is genuinely tricky:

* **Transfer buffer read**: write the 16-bit selector to `0x3E`, then *poll `0x3E`
  until the BQ echoes the selector back* (this proves the answer is ready), read the
  length from `0x61`, read the payload from `0x40`, read the checksum from `0x60`,
  and verify the checksum (`bq76952_selector_read_unlocked`).
* **Transfer buffer write**: write selector+payload to `0x3E`, then write
  checksum+length to `0x60` to commit (`bq76952_selector_write_unlocked`).
* **Optional CRC mode**: implement the BQ's per-byte CRC8 framing on reads and writes.
* **Retries**: up to `retries` (default 2) attempts with a short sleep.
* **Length validation**: reject obviously impossible lengths before trusting them.

If you ever have to debug "the BQ does not answer", this file is where you look. But
remember: **nothing above this file knows any of it exists.**

## 9.2 Semantic driver — "what does the register *mean*?"

`bq76952.c` translates raw registers into typed engineering values. Its output
structures are in `bq76952.h`:

```c
struct bq76952_measurements {
    uint8_t cell_count;
    int16_t cell_mv[16];   /* 16 cells, millivolts */
    int32_t stack_mv;      /* voltage across this AFE's cells */
    int32_t pack_mv;       /* BQ "PACK" pin voltage */
    int32_t ld_mv;         /* load/stack-top voltage */
    int32_t current_ma;    /* + = charge, - = discharge */
};
```

The functions it exposes are `bq76952_read_all_measurements`,
`bq76952_read_faults`, `bq76952_read_fet_state`, `bq76952_get_balance_mask`,
`bq76952_set_balance_mask`, and the four FET commands (`force_all_off`,
`force_charge_off`, `force_discharge_off`, `release_fets`).

The important conversions:

* **Cell voltage**: read the 16-bit direct command, treat as signed millivolts.
  (`cell()` at `bq76952.c:20`.)
* **Stack/PACK/LD voltage**: read a signed raw value, multiply by
  `user_volt_lsb_mv` (configured as **10 mV per LSB**) (`user_v()` at
  `bq76952.c:26`).
* **Current**: read CC2 as a signed raw value, multiply by
  `user_amp_lsb_ua / 1000`. The adapter configures **`user_amp_lsb_ua = 10000`
  (10 mA per LSB)** (`bq76952_read_all_measurements` at `bq76952.c:51`).
* **Faults**: map Safety Status A bits into the generic bitmap, and any non-zero PF
  status into `PERMANENT_FAIL` (`bq76952_read_faults` at `bq76952.c:58`).

> **Assumption to remember.** The 10 mV / 10 mA scalings are the values the BQ is
> assumed to be programmed with. They are **not read back from the chip**. On real
> hardware they must match the actual BQ Data Memory configuration, or every voltage
> and current will be wrong by a constant factor. This is called out in the manual's
> Known Gaps and in this book's mistakes chapter.

## 9.3 The BQ adapter — "make it look like any other AFE"

`bms_monitor_bq76952.c` is the thin translation layer that fulfils the generic
`bms_monitor_ops` contract using the semantic driver. It is small on purpose:

* `rd_sample` calls `bq76952_read_all_measurements` and fills a `bms_monitor_sample`,
  computing min/max cell values and their 0-based indices.
* `rd_faults` converts the BQ fault struct into `bms_monitor_fault_state`.
* `rd_switch` reads the FET status byte and decodes CHG/PCHG/DSG/PDSG.
* `set_switch` maps the four generic requests onto the four BQ subcommands:
  `RELEASE → ALL_FETS_ON`, `CHARGE_OFF → CHG_PCHG_OFF`,
  `DISCHARGE_OFF → DSG_PDSG_OFF`, `ALL_OFF → ALL_FETS_OFF`.
* `get/set_balance_mask` read/write the 16-bit "active cells" mask.

It also **advertises capabilities** and declares itself a 16-cell monitor
(`bms_monitor_bq76952.c:104-118`):

```c
bq76952_init(&x->bq, id, 16U /*cells*/, 10U /*mV/LSB*/, 10000U /*uA/LSB*/);
x->monitor.cell_count = 16U;
x->monitor.capabilities =
    BMS_MON_CAP_CELL_VOLTAGE | BMS_MON_CAP_STACK_VOLTAGE |
    BMS_MON_CAP_PACK_VOLTAGE | BMS_MON_CAP_CURRENT |
    BMS_MON_CAP_LOCAL_PROTECTION | BMS_MON_CAP_BALANCING |
    BMS_MON_CAP_SWITCH_CONTROL;
```

The hard-coded 16 is why a 19S pack is not a "just change a number" task yet — see
Known Gaps.

## 9.4 The generic monitor API itself

`bms_monitor.c` is trivial and defensive: every function checks that the monitor and
its `ops`/`backend` pointers are non-NULL, then forwards to the function pointer,
returning `-ENOTSUP` if the chip does not implement that operation. This is the
"capability" pattern: ask first with `bms_monitor_has_capability`, then call.

---

# 10. Layer 2: topology — "many AFEs, one battery"

A 32S pack is two 16-cell AFEs. The rest of the firmware does **not** want to know
that. Topology's job is to make two monitors look like one 32-cell battery.

## 10.1 The segment model

Topology is described by a small data structure (`bms_topology.h`):

```c
struct bms_topology_segment_cfg {
    struct bms_monitor *monitor;    /* which AFE */
    uint8_t local_cell_offset;      /* first cell inside that AFE (0 here) */
    uint8_t cell_count;             /* how many cells it contributes (16) */
    uint8_t global_cell_offset;     /* where those cells land in the pack (0 or 16) */
};

struct bms_topology_config {
    const char *name;
    const struct bms_topology_segment_cfg *segments;
    uint8_t segment_count;
    struct bms_monitor *current_provider;      /* who measures pack current */
    struct bms_monitor *pack_voltage_provider; /* who measures whole-pack volts */
    struct bms_monitor *switch_provider;       /* who drives the FETs */
};
```

The three "provider" pointers are the clever part. Not every quantity is equally
available from every AFE, so the topology explicitly names who owns each one:

| Quantity | 16S | 32S | Why |
|---|---|---|---|
| **Current** | BQ0 | BQ0 (bottom) | Only the bottom AFE sits on the shunt in the stacked design. |
| **Pack voltage** | BQ0 | *none* → topology sums stacks | A single stacked AFE cannot see the whole 32S voltage. |
| **Switch/FET** | BQ0 | BQ1 (top) | The high-side FET stage is driven by the top AFE. |

## 10.2 What `bms_topology_init` guarantees

`bms_topology_init` (`bms_topology.c:23`) refuses to accept a nonsensical map. It
verifies:

* each segment has a real monitor with ops/backend;
* the segment's requested cells fit inside the monitor (`local_cell_offset +
  cell_count ≤ monitor->cell_count`);
* global positions do not overlap (`used[]` array → `-EEXIST`);
* there are **no gaps** in the global cell numbering up to the highest used cell
  (`-EINVAL`);
* each provider is actually a member of this topology and actually has the required
  capability (`-ENOTSUP` if it claims pack voltage but cannot measure it).

If any check fails, the topology never initialises, so the device never runs. This
"fail loudly at startup, not mysteriously at runtime" style is used throughout the
project.

## 10.3 What `bms_topology_read_snapshot` does

This is the heart of topology (`bms_topology.c:138`). Step by step:

1. Read a `bms_monitor_sample` from **every** segment's monitor.
2. Copy each local cell into `snap.cell_mv[global]`, accumulating the running
   min/max cell and their global indices.
3. Sum the local `stack_mv` values into `snap.stack_mv`; if *any* segment's stack is
   invalid, mark the summed stack invalid too (fail closed).
4. For every segment that has `LOCAL_PROTECTION`, OR its `alerts`, `faults`, and
   `permanent_fail` into the pack-level ones.
5. Fill `current` from the **current provider** (error `-ENODATA` if it is invalid —
   the pack is treated as unmeasurable rather than guessed).
6. Fill `pack_voltage` from the **pack-voltage provider**; if that provider is NULL
   and the summed stack is valid, use the summed stack as the logical pack voltage.
   This is the 32S path.
7. Read the aggregated balance mask (below).
8. Read the switch state from the **switch provider**.

Two consequences you should be able to explain:

* **Currents are never summed** across stacked AFEs. Summing would double-count and
  is physically meaningless (the same current flows through the whole series
  string). The configured `current_provider` is used instead.
* **A missing/invalid pack voltage does not silently become zero.** The snapshot
  carries a validity flag with every measurement, so upper layers can distinguish
  "0 V" from "unknown".

## 10.4 Balance mask mapping

Balancing is per-cell. The policy above thinks in **global** 32-bit masks; each BQ
only understands a **local** 16-bit mask. Topology translates in both directions:

* `bms_topology_get_balance_mask` reads each monitor's local mask and scatters bits
  into global positions (`bms_topology.c:63`).
* `bms_topology_set_balance_mask` splits the global mask into per-segment local
  masks and calls each monitor (`bms_topology.c:89`). It also rejects any global bit
  beyond the total cell count.

So "balance global cells 3, 18, 30" becomes "BQ0 local 3" + "BQ1 local 2 and 14".

## 10.5 Topology is hardware, not user configuration

Notice that topology is **not** part of the persistent config object. It is fixed at
build/init time by `main_native.c` and `main_stm32.c`. That is deliberate: which AFE
is wired to which cells is a property of the PCB, not a field a user should casually
change. (A future product could still parameterise it per build profile.)

---

# 11. Layer 3: the snapshot — one coherent picture

The snapshot is the contract between "the hardware world" and "the BMS world". Its
full definition (`bms_snapshot.h`):

```c
struct bms_snapshot {
    uint32_t sequence;          /* increments on every successful publish */
    int64_t  captured_at_ms;    /* stamped by measurement at publish time */
    uint8_t  monitor_count;     /* how many AFEs contributed */
    uint8_t  cell_count;

    int32_t  cell_mv[CONFIG_BMS_MAX_CELLS];  /* global cell voltages */
    int32_t  cell_sum_mv;                    /* sum of cells */

    bool     stack_voltage_valid; int32_t stack_mv;
    bool     pack_voltage_valid;  int32_t pack_mv;
    bool     current_valid;       int32_t current_ma;

    int32_t  min_cell_mv, max_cell_mv;
    uint8_t  min_cell_index, max_cell_index; /* 0-based */

    uint64_t alerts, faults;    /* generic bitmaps (see Appendix B) */
    bool     permanent_fail;

    uint64_t balance_mask;      /* global, 0-based bits */

    bool     switch_state_valid;
    struct bms_monitor_switch_state switch_state;
};
```

Three properties give the snapshot its power:

1. **It is whole-pack.** All cells, one current, one pack voltage. No consumer has to
   know about segmentation.
2. **It is sequence-numbered.** `sequence` lets consumers detect "new data" (the
   estimator does exactly this) and lets events reference the exact picture that
   caused them.
3. **Every risky field carries a validity flag.** "Not measured" is different from
   "measured as zero". Protection and balancing both check these flags.

## 11.1 Why a snapshot instead of "read on demand"?

Imagine protection read the cells directly, CAN read them again, and the estimator a
third time. Each read takes milliseconds and the battery changes in between. Now
protection might decide using cell 7 = 2.79 V while the dashboard shows cell 7 =
2.85 V. Confusing, hard to diagnose, and in the worst case unsafe. The snapshot makes
one picture and everyone shares it.

---

# 12. Layer 4: measurement — the single publisher

`bms_measurement` owns acquisition and publication. Key fields
(`bms_measurement.h`): the topology pointer, a mutex, a delayable work item, an
`atomic_t running`, an `atomic_t valid`, the period, last error, last update time,
and the cached `latest` snapshot.

## 12.1 The critical design choice: read outside the lock

Look at `publish_once` (`bms_measurement.c:11`):

```c
/* acquire the NEXT snapshot WITHOUT holding the lock (slow I2C) */
ret = bms_topology_read_snapshot(m->topology, &next);
if (ret != 0) { record error; return ret; }

next.captured_at_ms = k_uptime_get();

/* now publish atomically under the lock */
k_mutex_lock(&m->lock, K_FOREVER);
m->latest = next;
m->last_update_ms = next.captured_at_ms;
m->last_error = 0;
atomic_set(&m->valid, 1);
k_mutex_unlock(&m->lock);
```

This means readers always get either the **previous complete snapshot** or the
**new complete snapshot**, never a half-updated one, and never block for the whole
I2C exchange. This is a classic "build offline, swap under lock" pattern and is worth
being able to explain.

## 12.2 Periodic vs manual

`measurement_work_handler` runs the acquisition, then reschedules itself for
`period_ms` if still running. The service supports:

* `bms_measurement_start/stop` — the periodic loop;
* `bms_measurement_set_period` — change cadence (reschedules if running);
* `bms_measurement_refresh_now` — one synchronous acquisition (used by self-test and
  the shell);
* `bms_measurement_get_latest` — copy the cache, **no hardware read**;
* `bms_measurement_has_snapshot`, `..._snapshot_age_ms`, `..._last_error`.

Default period is **200 ms**; runtime period is **100 ms**; a snapshot older than
**1000 ms** is considered stale.

## 12.3 A subtle bug-shaped detail (and why it is okay)

The runtime ticks faster than measurement, so between two publishes the runtime sees
the same sequence number twice. That is expected and handled: the estimator ignores
duplicate sequences, and protection happily re-evaluates the same snapshot (it is
idempotent and its hysteresis is stateful). The only "error" that can appear is a
transient `-ENODATA` at the very first runtime tick before the first publish; it
clears on the next tick.

---

# 13. Layer 5: protection — the decision, not the action

Protection is a **pure decision layer**. Given a snapshot and its age, it answers two
booleans: *is charging allowed?* and *is discharging allowed?* It never touches
hardware. This separation is what lets the same protection code run in simulation
and on hardware.

## 13.1 Inputs and outputs

```c
struct bms_protection_config {
    int32_t cell_uv_trip_mv, cell_uv_release_mv;
    int32_t cell_ov_trip_mv, cell_ov_release_mv;
    int32_t charge_oc_trip_ma, charge_oc_release_ma;
    int32_t discharge_oc_trip_ma, discharge_oc_release_ma; /* magnitudes */
    uint32_t max_snapshot_age_ms;
};

struct bms_protection_decision {
    uint32_t snapshot_sequence;
    uint64_t reasons;          /* bitmap of WHY */
    bool charge_allowed;
    bool discharge_allowed;
};
```

The `reasons` bitmap has two halves (`bms_protection.h:13`):

* **Software reasons (bits 0–4)**: computed by firmware from measurements —
  `SW_CELL_UV`, `SW_CELL_OV`, `SW_CHARGE_OC`, `SW_DISCHARGE_OC`, `STALE_SNAPSHOT`.
* **Hardware reasons (bits 16–21)**: translated from the AFE's own fault bits —
  `HW_CELL_UV`, `HW_CELL_OV`, `HW_CHARGE_OC`, `HW_DISCHARGE_OC`,
  `HW_SHORT_CIRCUIT`, `HW_PERMANENT_FAIL`.

Keeping the two namespaces separate matters: when a fault appears you can immediately
tell whether it came from *your* thresholds or from the *chip's* hardware comparator.

## 13.2 Hysteresis, explained without jargon

If you trip at exactly 2800 mV and release at exactly 2800 mV, a cell hovering at
2799–2801 mV will chatter: block, allow, block, allow. Protection solves this with
**hysteresis**: trip *below* 2800, but only release *above* 3000. Between 2800 and
3000, nothing changes; the previous state is held.

`update_hysteretic_bit` (`bms_protection.c:36`) implements exactly this for each
bit:

```c
if (!active && trip_condition)      bits |= bit;   /* turn on  */
else if (active && release_condition) bits &= ~bit; /* turn off */
```

Each software reason is stateful in `p->active_software_reasons`, so it "remembers"
it is tripped across evaluations.

## 13.3 Building the decision

After updating the software bits, protection adds the hardware bits from the
snapshot's generic fault bitmap, then applies two masks (`bms_protection.c:115`):

```text
Charge is blocked by:    OV, charge-overcurrent, stale snapshot,
                         HW OV, HW charge-OC, permanent failure.

Discharge is blocked by: UV, discharge-overcurrent, stale snapshot,
                         HW UV, HW discharge-OC, HW short circuit,
                         permanent failure.
```

Notice what is **deliberately absent**: nothing blocks charge on UV (you *want* to
charge an empty battery), nothing blocks discharge on OV (you *want* to drain a full
one). Only the direction that makes the condition worse is blocked. A **stale
snapshot blocks both** — that is the fail-safe.

## 13.4 Default (development!) thresholds

| Setting | Trip | Release |
|---|---:|---:|
| Cell UV | 2800 mV | 3000 mV |
| Cell OV | 3650 mV | 3500 mV |
| Charge OC | +30000 mA | +25000 mA |
| Discharge OC magnitude | 50000 mA | 40000 mA |

Every one of these is a **development default** from `bms_config.c:24`, not a
product limit. Say this out loud whenever you present the project.

---

# 14. Layer 6: the operating state machine and switch authority

Protection says what is allowed. The **state machine** turns that into one of five
states and one generic switch request. It is the *only* place that decides what to do
to the FETs.

## 14.1 States and mapping

| Charge allowed | Discharge allowed | State | Switch request |
|---|---|---|---|
| yes | yes | `NORMAL` | `RELEASE` |
| no | yes | `CHARGE_BLOCKED` | `CHARGE_OFF` |
| yes | no | `DISCHARGE_BLOCKED` | `DISCHARGE_OFF` |
| no | no | `ALL_BLOCKED` | `ALL_OFF` |
| — | — | `INIT` | `ALL_OFF` |

Implementation: `target_from_decision` and `request_from_state`
(`bms_state_machine.c:5`).

## 14.2 The meaning of `RELEASE` (easy to get wrong)

`RELEASE` does **not** mean "force the FETs on". It means **"release any
host-forced inhibition"** — i.e. stop the MCU from holding the FETs off, and let the
BQ's own hardware protection have final authority. On the BQ this is the
`ALL_FETS_ON` subcommand. If the chip itself is protecting, `RELEASE` will not
override it. This distinction is important for safety and is a great interview
detail.

## 14.3 Actuation gating

The state machine holds an `actuation_enabled` flag. Even when the state changes, it
only calls `bms_topology_set_switch` if that flag is true
(`bms_state_machine.c:69`). On STM32 bring-up builds the flag is **false**, so the
whole decision chain runs and logs state changes, but no FET is touched. State
changes are still published as events, so you can watch the logic work safely.

## 14.4 Forcing safe outputs

`bms_state_machine_force_all_off` (`bms_state_machine.c:110`) is the supervisor's
big red button: it sends `ALL_OFF` (if actuation is enabled) and drives the state to
`ALL_BLOCKED`, publishing a state-change event. The supervisor calls it when it
latches safe.

---

# 15. Layer 7: balancing — equalising the cells

Balancing is a second, independent consumer of the same snapshot and protection
decision. It produces a **global desired balance mask**; topology routes it to the
AFEs.

## 15.1 The policy in words

The rule of thumb: *only bleed a cell when it is both high in absolute terms and
high relative to the pack, and only when it is safe to do so.*

The policy first accumulates **inhibit reasons** (`bms_balancing.c:94`):

| Inhibit | Meaning |
|---|---|
| `DISABLED` | balancing switched off |
| `PROTECTION` | any protection reason active or a direction blocked |
| `CURRENT_INVALID` | no valid current reading |
| `CURRENT_HIGH` | \|current\| above `max_abs_current_ma` |
| `VOLTAGE_LOW` | max cell below `start_cell_mv` |
| `DELTA_SMALL` | spread too small |

The first five are **hard** inhibits: balancing is simply off. Only if none apply
does it compute eligibility.

## 15.2 Selecting cells, with per-cell hysteresis

For each cell (`bms_balancing.c:132`):

* if it was active last time, it stays eligible while `mv ≥ start_cell_mv` **and**
  `delta > stop_delta_mv`;
* if it was not active, it becomes eligible only when `mv ≥ start_cell_mv` **and**
  `delta ≥ start_delta_mv`.

Then if more cells are eligible than `max_simultaneous_cells`, the
**highest-voltage** ones win (`select_highest_cells`, `bms_balancing.c:47`). The
previous desired mask is stored for the hysteresis.

## 15.3 Defaults

| Setting | Default |
|---|---:|
| Enabled | yes |
| Start cell voltage | 3400 mV |
| Start delta | 20 mV |
| Stop delta | 10 mV |
| Max \|pack current\| | 5000 mA |
| Max simultaneous cells | 4 |

The 4-cell cap is a thermal/power limit: each bleed resistor dissipates real heat.
The current cap stops you balancing while a big load is connected (the resistor
would be fighting the load).

---

# 16. Layer 8: the estimator — SOC, SOH, and counters

The estimator is deliberately a *peer* of protection and balancing, not their boss:
it consumes the same published snapshot and never reads hardware. SOC does **not**
(yet) feed back into protection. That coupling is intentionally deferred until a
product needs it.

## 16.1 Units and sign conventions (memorise these)

* **Current**: `+` = charging, `-` = discharging. Always.
* **SOC**: "centi-percent". `10000` = 100.00 %, `5000` = 50.00 %, `7350` = 73.50 %.
* **Capacity**: config in **mAh**; internal counters in **µAh** (1000× smaller) to
  preserve precision.
* **SOH**: centi-percent, like SOC (`10000` = 100.00 %).
* **EFC**: equivalent full cycles, in **milli-cycles** (`1000` = 1.000 cycle).

## 16.2 The integration, exactly

From Chapter 5 you already know the physics. Here is how the code keeps it exact
despite integer math (`bms_estimator.c`):

* For SOC: `delta_uAh = (I[mA] × dt[ms] × efficiency) ÷ 3,600,000`.
  Efficiency is `charge_efficiency_permille` while charging, and exactly 1000
  (100 %) while discharging.
* The **remainder** of every division is carried into the next step
  (`charge_remainder_scaled`), so many tiny steps still add up correctly.
* For charge counters: `delta_uAh = |I| × dt ÷ 3600` with its own remainder.
* For energy: `delta_uWh = V[mV] × |I|[mA] × dt[ms] ÷ 3,600,000` with its own
  remainder. Energy-in uses positive current, energy-out uses negative current, and
  energy is only accumulated when the pack voltage is valid and positive.
* After each step, percentages are recomputed, clamping remaining charge into
  `[0, full]` and SOC into `[0, 10000]`.

## 16.3 What it refuses to do

* **No duplicate integration**: same `sequence` twice → ignored.
* **No gap integration**: `dt ≤ 0`, `dt > max_integration_interval_ms`, or invalid
  current → count `skipped_intervals++` and re-anchor.
* **No fake SOH**: there is **no automatic capacity-learning algorithm**. SOH is
  simply:

  ```text
  SOH = estimated_full_capacity_mAh ÷ design_capacity_mAh
  ```

  The `estimated_full_capacity_mAh` can be restored from persistence or set by a
  future learner/calibration, without touching CAN, RS485, or topology.

## 16.4 Counters tracked

Remaining charge (µAh), charged (µAh), discharged (µAh), throughput (µAh), energy-in
(µWh), energy-out (µWh), and EFC (milli-cycles, computed from discharged charge over
estimated full capacity). `bms_estimator_reset_counters` clears the counters but
leaves SOC/charge untouched.

## 16.5 Persistence of estimator state

`bms_estimator_persisted_state` deliberately saves only *useful* values: remaining
charge, estimated full capacity, the counters, and the skipped-interval count. It
does **not** save timing anchors or fractional remainders — after a restore the
estimator simply re-anchors on the next snapshot. This is a clean, defensible design
choice: you never persist a stale "last timestamp" that would cause a huge false
integration after reboot.

---

# 17. Layer 9: configuration and persistence

## 17.1 Configuration is a domain object now

Before this revision, thresholds were `static const` values baked into `main.c`.
Now they live in one struct (`bms_config.h`):

```c
struct bms_app_config {
    uint16_t schema_version;
    uint32_t measurement_period_ms;
    uint32_t runtime_period_ms;
    struct bms_protection_config protection;
    struct bms_balancing_config    balancing;
    struct bms_estimator_config    estimator;
};
```

Topology is *not* in here (it is hardware). Estimator *runtime state* is also not
here (it is persisted separately).

## 17.2 Validation is real, not decorative

`bms_config_validate` (`bms_config.c:86`) enforces a coherent threshold set, e.g.:

* UV release > UV trip > 0 (otherwise the hysteresis is backwards).
* OV trip > OV release > UV release (sane ordering).
* charge OC trip > release ≥ 0; discharge similarly.
* `max_snapshot_age_ms ≥ 2 × measurement_period_ms` (so one late cycle cannot
  instantly trip "stale").
* balancing: `start_delta > stop_delta`, etc.
* estimator: nonzero capacities, efficiency in `(0, 1000]`.

Invalid configuration is rejected before it can change behaviour.

## 17.3 Staged vs applied — why it exists

A multi-field edit (change UV trip *and* release *and* periods) must not take effect
half-way. So edits go to a **STAGED** config. Only `bms config apply` validates the
whole staged object and swaps it in as the **APPLIED** config. On the STM32, the
apply path also stops measurement+runtime, reconfigures them, and restarts them in a
controlled order.

## 17.4 The persistent image format

The project never memcpy's a C struct to flash (that breaks the moment you recompile
with a different layout). Instead it serialises fields explicitly
(`bms_config_store.c`):

```text
+---------------------------------------------------+
| magic (4)  | version (2) | payload_len (2)         |  16-byte header
| generation (4) | CRC-32 (4)                      |
+---------------------------------------------------+
| payload: bms_app_config + optional estimator state |
+---------------------------------------------------+
```

* **magic** `0x434D5342` ("BMSC") rejects garbage.
* **version** rejects incompatible future/old formats.
* **payload_len** must equal `image_len - header` (detects truncation).
* **generation** increments each save (diagnostic).
* **CRC-32** (standard reflected poly `0xEDB88320`) detects corruption.

Encoding is little-endian field by field. The whole current image is only ~153
bytes, tiny compared with the 256-byte buffer.

## 17.5 Backends: the same interface, three implementations

The store talks to a backend through three operations: `read`, `write`, `erase`.

| Backend | File | Persists across | Use |
|---|---|---|---|
| Virtual RAM | `bms_config_store_virtual.c` | simulated reboot in-process | native sim, STM32 default |
| STM32 NVS | `platform/stm32/bms_config_store_nvs.c` | power cycles | real flash (disabled by default) |

The STM32 NVS backend mounts Zephyr's NVS over a fixed flash partition. It is off by
default because the flash map must be approved first (see
`overlays/stm32f446_storage_nvs.overlay`, which reserves `0x40000–0x7FFFF` on a
512 KiB part).

The native backend's "simulate reboot" is the clever bit: `bms config save` writes to
RAM, `bms config simulate_reboot` reloads it and rebuilds the estimator as if the MCU
had reset — proving the whole persistence + restore path without hardware.

---

# 18. Layer 10: events, protocols, communications, and the supervisor

## 18.1 The event log

`bms_events` is a fixed-size RAM ring buffer (default 32 records) protected by a
mutex. Each record has an id, timestamp, type, snapshot sequence, a 64-bit `data`
payload, and a 32-bit `detail`. Types include `PROTECTION_CHANGED`, `STATE_CHANGED`,
`ACTUATION_ERROR`, `BALANCE_CHANGED`, `SELF_TEST`, `SYSTEM_MODE_CHANGED`,
`SYSTEM_SAFE_LATCHED`, `SYSTEM_RECOVERED`.

The important sentence from the architecture doc: **events are diagnostic history,
not a safety source of truth.** If the event log is full, you lose history, not
protection.

## 18.2 The communications service

`bms_communications` is above the BMS core and below the outside world. It builds one
**telemetry view** for each transmission (`bms_communications.c:18`) by combining:

* the latest published snapshot,
* the latest protection decision,
* the authoritative state-machine state,
* the latest estimator status.

It then encodes that view into frames and hands the bytes to a **backend**:

| Backend | Ops | Implementation |
|---|---|---|
| CAN | `send(frame)` | virtual ring buffer / Zephyr `can_send` |
| RS485 | `send(bytes)` | virtual last-frame buffer / UART + DE/RE |
| Config | `get_applied/stage/apply/save` | provided by the application main |

The protocol encoder never reads hardware, and the backends never decide policy. A
CAN period of 500 ms is the default.

## 18.3 The CAN reference profile

**This is a STARZ reference protocol, not Pylontech/Victron/J1939/CANopen.** For a
32-cell pack the encoder emits **13 frames** per cycle: 5 fixed + 8 cell frames.

| ID | Content |
|---|---|
| `0x500` | pack voltage (10 mV/LSB, u16), current (100 mA/LSB, i16), SOC (0.1 %/LSB, u16), state (u8), flags byte |
| `0x501` | min cell mV, max cell mV, min index+1, max index+1, cell count, reserved |
| `0x502` | low 32 bits of faults + low 32 bits of balance mask |
| `0x503` | high 32 bits of faults + high 32 bits of balance mask |
| `0x504` | SOH (0.1 %/LSB), estimated full capacity (100 mAh/LSB), EFC (milli-cycles, u32) |
| `0x510+` | cells 1–4, 5–8, … four direct-mV u16 per frame |

Flags byte of `0x500`: bit0 charge allowed, bit1 discharge allowed, bit2 CHG switch
on, bit3 DSG switch on.

## 18.4 The RS485 framed protocol

Wire format:

```text
A5 5A | version(1) | type(1) | seq(2, LE) | len(2, LE) | payload(0..192) | CRC16(2, LE)
```

CRC is **CRC16-CCITT-FALSE**, poly `0x1021`, init `0xFFFF`, covering from *version*
through the *payload* (excluding SOF and CRC). Message types:

| Value | Request | Value | Response |
|---:|---|---:|---|
| `0x10` | status | `0x90` | status |
| `0x11` | cells | `0x91` | cells |
| `0x12` | config | `0x92` | config |
| `0x20` | set staged config | `0xA0` | ACK |
| `0x21` | apply staged config | `0xA1` | error |
| `0x22` | save config | | |

**Remote config writes are policy.** They are enabled in `native_sim` to exercise
the path and **disabled by default on STM32**, because there is no authentication
yet. Never expose an unauthenticated write path on a real product.

## 18.5 The system supervisor

The supervisor owns health, safe-latching, and watchdog feeding. Five modes
(`bms_system_policy.h`):

```text
BOOTING → READY → DEGRADED (comm problems only)
                 SAFE_LATCHED (battery-control failure; latched)
                 MAINTENANCE (controlled reconfiguration)
```

**Health flags** split into two groups:

* **Critical** (→ SAFE_LATCHED): self-test failed; no snapshot; measurement
  stale/error/stopped/no-progress; runtime error/stopped/no-progress.
* **Warning** (→ DEGRADED): communication error/stopped/no-progress.

The classification is pure and unit-tested (`bms_system_policy.c:23`, and
`tests/system/test_system_policy.c` asserts each branch).

**Startup self-test** (`bms_system.c:208`) checks: applied config validates;
topology exists with a cell count; a fresh measurement can be acquired; one core
protection/estimator cycle works; a telemetry view can be collected; the store is
readable (an *empty* store is explicitly OK on first boot).

**Safe latch** (`enter_safe_latched`, `bms_system.c:28`): stop the periodic runtime,
force safe outputs (clear balance, `ALL_OFF`), publish an event, and set
`safe_latched`. It never clears by itself. **Recovery is explicit**
(`bms_system_recover`): it restarts measurement, refreshes, processes once, restarts
runtime and communications, and only then clears the latch.

**Watchdog policy** (`bms_system.c:327`): the supervisor feeds the watchdog only
when self-test passed and the system is not safe-latched. A communication-only
degraded state does **not** stop feeding, because battery control is still healthy —
a deliberate, arguable choice worth being able to defend.

---

# 19. The native simulator: the software regression environment

`src/main_native.c` is huge, but it has one simple job: **wire the same services
together on top of the virtual BQ model, then expose a shell to poke and test them.**

## 19.1 What it instantiates

```text
virtual_bq_reset()               -> BQ0 baseline 3300 mV-ish, BQ1 3350 mV-ish
2 x bms_monitor_bq76952          -> one per virtual AFE
topology                         -> 2 segments, 32 cells
                                 -> current=BQ0, pack=BQ0(!), switch=BQ1
measurement (200 ms)             -> periodic publisher
core runtime (100 ms)            -> protection + estimator + balancing + state
communications (500 ms)          -> virtual CAN + virtual RS485
config store (virtual RAM)
system supervisor (250 ms)       -> virtual watchdog
```

> **Note the `pack=BQ0(!)`.** The simulator currently names BQ0 as the pack-voltage
> provider, which is *not* the same choice the STM32 32S profile makes (that one sums
> both stacks). This inconsistency is discussed in the mistakes chapter; it is the
> most substantive issue found during this review.

## 19.2 The virtual BQ model

`tests/virtual_bq/virtual_bq_model.c` is a tiny in-RAM AFE: 16 cell values (two
models with slightly different starting voltages), a current, fault bytes, an FET
status byte, a balance mask, and a Data Memory byte. It initialises the FETs to
CHG+DSG on and DA configuration to `0x06`.

`bq76952_transport_virtual.c` implements the **same transport functions** against
this model. That means the semantic driver, adapter, topology, and everything above
are *the real code*, not test doubles. Only the very bottom is fake. This is exactly
why `bms test all` is meaningful.

## 19.3 What the shell can do

The simulator exposes the full engineering shell (see the command reference in the
appendix). Highlights:

* Layer tests: `bms test transport|driver|monitor|topology|measurement|runtime|
  balancing|estimator|persistence|communications|system|all`.
* Behaviour tests: `bms scenario all`.
* Live state: `bms snapshot`, `bms state`, `bms protection status`,
  `bms balancing status`, `bms estimator status`, `bms system status`.
* Fault injection: `bms inject cell <bq> <cell> <mV>`,
  `bms inject current <bq> <mA>`, `bms inject uv <bq> on|off`.
* Config: `bms config show|set|apply|save|load|simulate_reboot|erase`.
* Communications: `bms comm can publish|dump`,
  `bms comm rs485 request|stage_uv|apply|save|dump`.

## 19.4 The regression scenarios, in order

`run_regression_scenarios` (`main_native.c:300`) deliberately stops all periodic
loops (so the test is deterministic), turns on maintenance mode, then drives the
virtual BQ through a scripted sequence and asserts the resulting protection decision:

1. **NORMAL** — baseline, charge and discharge allowed.
2. **SOFTWARE_UV** — drop a cell below UV trip → discharge blocked, `SW_CELL_UV` set.
3. **SOFTWARE_OV** — raise a cell above OV trip → charge blocked, `SW_CELL_OV` set.
4. **DISCHARGE_OC** — large negative current → discharge blocked.
5. **CHARGE_OC** — large positive current → charge blocked.
6. **HARDWARE_UV** — set the virtual BQ safety bit → `HW_CELL_UV` appears, proving
   the hardware-fault path.
7. **BALANCING** — create a cell spread → balancing desires a non-empty mask.
8. **COMMUNICATIONS** — CAN publish and RS485 status round-trip.

At the end it resets the model and restarts whichever services were running. This is
the single best thing to run before any demo.

---

# 20. Test evidence: what is actually proven

`VALIDATION.txt` records host tests that pass under `gcc -std=c11 -Wall -Wextra
-Werror`:

```text
PASS: protection policy host tests
PASS: balancing policy host tests
PASS: estimator host tests
PASS: estimator persisted-state host tests
PASS: config + persistence store host tests
PASS: CAN + RS485 protocol host tests
PASS: system health policy host tests
```

The host tests in `tests/` (e.g. `tests/protocol/test_protocol.c`) compile the pure
modules *without Zephyr* and assert exact behaviour — including corruption
rejection by CRC, 13-frame CAN layout, and config round-trips. Beyond those, the
native `bms test all` exercises the integrated stack.

**The honest boundary:** these prove *software behaviour*. They do **not** prove
anything electrical. The final STM32 builds must still be compiled in a real Zephyr
workspace, and the hardware must be validated per Chapter 22. Never claim otherwise.

---

# PART IV — HARDWARE AND BUILDS

# 21. The two STM32 hardware profiles

Everything in this part is **the contract the firmware expects from a PCB**, and
everything except the 16S pins is derived from the legacy CubeMX project
(`reference/VS_BMS_16S_reference.ioc`). The 32S pin additions are a *design
specification for a future board*, not verified wiring.

## 21.1 Common MCU and clock

* MCU: **STM32F446ZE** (LQFP144), Cortex-M4 @ 168 MHz.
* HSE: **8 MHz**. PLL M=4, N=168, P=2, Q=7 → **SYSCLK 168 MHz**.
* AHB ÷1, APB1 ÷4 (42 MHz), APB2 ÷2 (84 MHz).

All of this appears as DTS in the overlays (`&clk_hse`, `&pll`, `&rcc`) and matches
the legacy `.ioc` exactly (verified: `RCC.HSE_VALUE=8000000`, `PLLM=4`, `PLLN=168`,
`PLLQ=7`, `SYSCLK=168000000`, `APB1=÷4`, `APB2=÷2`).

## 21.2 Peripherals

| Function | Pins / peripheral | Notes |
|---|---|---|
| BQ76952 (bottom) | I2C1 **PB8/PB9**, 100 kHz, addr `0x08` | from the legacy design |
| BQ ALERT | **PC5** input | legacy `.ioc` wrongly had it as *output*; fixed here |
| BQ RST_SHUT | **PB14** output, inactive low | legacy |
| BQ DFETOFF | **PB6** output, inactive low | legacy |
| CAN | CAN1 **PD0/PD1**, 500 kbit/s, 87.5 % sample point | legacy |
| RS485 | UART4 **PC10/PC11**, 115200 | legacy |
| RS485 DE/RE | **PB2** output | legacy |
| ESP32 bridge | UART5 **PE8/PE7**, 115200 | legacy |
| Console/shell | USART3 **PD8/PD9**, 115200 | legacy |
| LCD | I2C3 **PA8/PC9** | legacy (unused by firmware today) |
| Watchdog | IWDG | enabled in both profiles |

Board extras (from the overlay, with legacy labels): LEDs on PC4/PF12/PA7/PF13,
buttons on PA5/PA6/PA4/PF14, buzzer on PD15, START on PF11. The legacy `.ioc` also
had `PF11=START` and `PC5=ALERT` declared as outputs; the Zephyr overlays correct
these to inputs per the product hardware map.

## 21.3 The 32S reference architecture (the interesting one)

```text
Cells 1..16  -> BQ0 (bottom) -> I2C1 PB8/PB9 ------------> STM32F446 (MCU ground)
Cells 17..32 -> BQ1 (top)    -> ISO1640 -> I2C2 PF1/PF0 --> STM32F446
```

The top AFE floats at the upper group's potential, so its digital side must cross an
**isolation/level-shift barrier**. The project names the **ISO1640** isolated I2C
part as the reference device. Because BQ0 and BQ1 are on *different MCU I2C
controllers*, both keep address `0x08`, avoiding OTP programming.

Provider roles (from `main_stm32.c:139-151`):

* current → **BQ0** (bottom, on the shunt);
* pack voltage → **NULL**, so topology sums BQ0.stack + BQ1.stack;
* switch/FET → **BQ1** (top, presumed high-side FET stage);
* faults → OR of both monitors;
* balancing → global mask split into two local masks.

Upper-monitor GPIO assignments (design requirements for the future PCB, routed
through an isolated/level-shifted GPIO path):

| Function | MCU pin | Direction |
|---|---|---|
| TOP_ALERT | PE12 | in to MCU |
| TOP_RST_SHUT | PB0 | out to top AFE |
| TOP_DFETOFF | PB7 | out to top AFE |

In the legacy 16S `.ioc` these three pins were already configured as outputs but
unlabelled/unused — consistent with the story that they become the 32S additions.

> **Hardware truths Devicetree cannot express** (from `docs/HARDWARE_32S.md`): the
> ISO1640 secondary supply must exist whenever you talk to the top AFE; the top AFE
> ground is floating; and **a fast bottom-AFE-to-top-FET hardware shutdown path is a
> PCB responsibility**. Firmware is not a substitute for the fast SCD/OCD path.

---

# 22. Building one codebase three ways

The root `Kconfig` defines two independent choices:

* **Platform**: `BMS_PLATFORM_NATIVE_SIM` or `BMS_PLATFORM_STM32F446`.
* **Pack profile**: `BMS_PACK_16S` or `BMS_PACK_32S`.

`CMakeLists.txt` uses those to decide which `main_*.c` and which backends compile:

```text
always:        all subsys/bms/*.c + bq76952.c + bms_monitor_bq76952.c
native_sim:    main_native.c + virtual store/comms/watchdog + virtual BQ model/transport
STM32:         main_stm32.c + i2c transport + board + comms backends
               + (watchdog backend if enabled) + (virtual store OR NVS backend)
```

Safety-related switches:

| Switch | native_sim | STM32 16S/32S | Meaning |
|---|---|---|---|
| `BMS_HW_ACTUATION_ENABLE` | y | **n** | when n, decisions run but FETs/balancing are not written |
| `BMS_REMOTE_CONFIG_WRITE` | y | **n** | remote RS485 config writes |
| `BMS_STM32_USE_HW_WATCHDOG` | — | y | use IWDG |
| `BMS_STM32_USE_NVS` | — | n | use real flash NVS |

This is the project's central selling point: **one BMS core, three products, chosen
by configuration and a data file — not by forks of the logic.**

---

# PART V — RUNNING, TESTING, EXTENDING

# 23. First hardware bring-up (the safe order)

Both STM32 profiles default to `CONFIG_BMS_HW_ACTUATION_ENABLE=n`. Do not change it
until every earlier step passes.

1. Build both STM32 profiles in a real Zephyr/NCS workspace and confirm they compile.
2. Confirm the clock tree and the USART3 console work.
3. Run `bms status`, `bms system`, `bms board`, `bms events`.
4. Confirm BQ0 answers: `bms bq_ping 0` (reads device number 0x7695).
5. On 32S confirm the isolated BQ1: `bms bq_ping 1`.
6. Verify I2C pull-ups and waveform quality on both buses.
7. Verify the ISO1640 upper supply and floating-ground implementation.
8. Verify ALERT polarity and interrupts on every monitor.
9. Verify `RST_SHUT` polarity and timing.
10. Confirm DFETOFF/BOTHOFF configuration before relying on the pin.
11. Program and verify BQ Data Memory for the real chemistry and pack.
12. Calibrate cell/stack/PACK voltage scaling and shunt/current scaling (and sign).
13. Verify hardware UV/OV/OCC/OCD/SCD response with controlled sources.
14. Verify FET OFF commands first with no high-energy load.
15. Verify balancing **one cell at a time**; measure current and temperature.
16. Verify CAN transceiver/bitrate/termination and RS485 DE/RE timing/termination.
17. Approve the NVS flash partition before enabling persistent flash writes.
18. Verify watchdog reset behaviour on real hardware.
19. Only then enable `CONFIG_BMS_HW_ACTUATION_ENABLE` and re-verify everything.
20. Final product validation: thermal, fault injection, pack-level, EMC, abuse,
    certification.

(Chapter 17 of the manual and Appendix C there contain the same checklist; it is
repeated here because it is the single most important operational list in the
project.)

---

# 24. Known gaps (do not accidentally claim these are done)

Straight from the code and the manual's Known Gaps:

* **No generic temperature channel.** The BQ has temperature commands and the driver
  header has TS1/TS2/TS3 addresses, but the generic monitor capability set, snapshot,
  protection, and telemetry do **not** carry temperature. Safety Status B/C are read
  but not mapped.
* **No automatic SOH capacity learning.** SOH is a capacity ratio only.
* **No variable cell count.** Both product initialisers build full 16-cell
  segments; 17–31S would need a real cell-mode/topology profile and BQ `VCellMode`
  configuration.
* **No complete BQ Data Memory programming/calibration sequence** in the STM32
  startup path.
* **NVS disabled by default** pending an approved flash map.
* **CAN is a STARZ reference profile**, not any inverter vendor profile.
* **32S upper GPIOs are a future-PCB specification.**
* **No physical/electrical validation yet**; host tests are not target tests.

---

# 25. How to extend it (and the rule to preserve)

The architecture's whole point is that extensions have one correct place to live:

| New feature | Where it goes |
|---|---|
| New AFE chip | implement `bms_monitor_ops`; add a transport; no changes above topology |
| Temperature | new monitor capability + snapshot field + protection reason + telemetry field (all coherently) |
| A real inverter CAN profile | a new encoder that consumes `bms_telemetry_view` |
| ESP32 port | new platform backends (I2C, CAN/TWAI, UART, flash, watchdog) + board DTS; keep the BMS core |
| Capacity-learning SOH | a new estimator function that updates `estimated_full_capacity_mAh` |
| Variable cell count | extend the topology/segment profile and BQ cell-mode config |

The **rule** to preserve, from the manual: *"If a future port requires edits deep
inside protection/topology merely because the MCU changed, the platform boundary has
been broken."*

---

# 26. Review: mistakes, risks, and suggested modifications

This chapter is the answer to *"if there are any mistakes or modifications you'd like
to perform, inform me."* It records every finding from the review. A subset of the
safe, high-confidence items has since been **fixed in the code**; those are marked
`[FIXED]` and listed in the change log at the end of this chapter (26.5). The rest are
deliberately left open because they are either feature work or need decisions from
you. Severity is my judgement. Line numbers are from the reviewed tree.

## 26.1 Confirmed issues (worth fixing)

### M1 — Native simulator uses the wrong 32S pack-voltage provider (medium)

* **Where:** `src/main_native.c:2214`
  (`.pack_voltage_provider = &bq0.monitor`).
* **What:** In the virtual 32S build, the pack voltage is taken from BQ0 alone, so it
  reflects only cells 1–16 (roughly **half** the true 32S pack voltage). The STM32
  32S profile correctly passes `NULL` so topology sums both stacks
  (`src/main_stm32.c:149`), and every document (including
  `docs/DEEPSEEK_PROJECT_CONTEXT.md`) says 32S pack voltage is the **sum**.
* **Impact:** In simulation, CAN `0x500` pack voltage, the estimator's energy-in/out
  integration, and any pack-voltage-based reasoning are ~half of reality. The
  existing `bms test topology` does not check pack voltage, so it passes while the
  number is wrong.
* **Suggested fix:** set `.pack_voltage_provider = NULL` in `main_native.c` to match
  the STM32 32S profile and the documented design; optionally add a `t_topology`
  assertion that `pack_mv ≈ stack_mv ≈ sum of both AFEs`.

### M2 — Safe-output path writes the balance mask even when actuation is disabled (low–medium)

* **Where:** `subsys/bms/bms_core_runtime.c:494`
  (`bms_topology_set_balance_mask(..., 0U)` in `bms_core_runtime_force_safe_outputs`).
* **What:** `bms_state_machine_force_all_off` respects `actuation_enabled`, but the
  balance-mask clear does not check `balancing_actuation_enabled` and is sent
  unconditionally.
* **Impact:** On a bring-up STM32 build (actuation disabled) a safe-latch still
  performs a BQ write. Writing `0` is electrically safe, but it contradicts the
  "no normal runtime writes during bring-up" intent, and a failing write makes
  `enter_safe_latched` return an error.
* **Suggested fix:** either guard the write with `rt->balancing_actuation_enabled`,
  or document explicitly that "balance mask 0" is a safety action that must always
  be attempted.

### M3 — Fault mapping is incomplete and conflates alerts with faults (low–medium)

* **Where:** `drivers/.../bq76952.c:58-83` and
  `drivers/.../bms_monitor_bq76952.c:46-47`.
* **What:** Safety Alert B/C (temperature) and Alert A bits outside the mapped set
  (e.g. AOLD, OCD3) are read but never used. Worse, the adapter sets `alerts` and
  `faults` to the **same** value (`out->alerts = raw.flags; out->faults = raw.flags;`),
  so the snapshot's `alerts` field is not a distinct live-alert bitmap.
* **Impact:** No temperature protection, and consumers cannot distinguish "live
  alert" from "latched status". This overlaps the documented temperature gap; the
  alerts/faults conflation is a code-level issue.
* **Suggested fix:** when temperature is added, populate `alerts` from Safety Alert
  A/B/C and `faults` from Safety Status A/B/C, and extend `bms_fault_flag` with the
  new bits.

### M4 — Hard-coded AFE geometry and scaling (documented gap, but a trap)

* **Where:** `drivers/.../bms_monitor_bq76952.c:104,110` (16 cells, 10 mV/LSB,
  10 mA/LSB) and `drivers/.../bq76952.c:26,54`.
* **What:** The adapter always declares 16 cells and fixed LSBs; these are assumed,
  never read back from the BQ's Data Memory.
* **Impact:** Any pack that is not exactly 16 cells, or any BQ not programmed to
  those LSBs, is miscalculated by a constant factor. 17–31S is not supported.
* **Suggested fix:** read the relevant calibration/LSB values (or make them
  per-instance configuration) and derive `cell_count` from the product profile.

### M5 — No BQ Data Memory configuration (CONFIG_UPDATE) sequence (documented gap)

* **Where:** `bq76952_dm_write` exists (`bq76952_transport_i2c.c:611`) but the
  `SET_CFGUPDATE (0x0090)` / `EXIT_CFGUPDATE (0x0092)` subcommands defined in
  `bq76952_reg.h:106-107` are never used.
* **Impact:** The firmware does not program the chip's own UV/OV/OCC/OCD/SCD,
  temperature, or FET settings. Those must currently be set by TI's tool. A real
  product would need a controlled programming sequence (enter config-update, write,
  verify, exit, checksum).
* **Suggested fix:** implement a versioned, verified Data Memory programming step
  invoked at startup and guarded by a build switch, keeping values in configuration,
  not in the register map.

### M6 — RS485 transmit tail timing is fragile (medium, hardware only)

* **Where:** `platform/stm32/bms_communications_stm32.c:27-43`.
* **What:** After `uart_poll_out` of the last byte, TX-enable is dropped after
  `k_busy_wait(120)`. At 115200 baud one byte is ~87 µs, and `uart_poll_out`
  generally only guarantees the byte reached the peripheral, not that it fully
  shifted out of the transceiver.
* **Impact:** Risk of truncating the last byte or of releasing the bus early,
  especially at other baud rates or clock configurations.
* **Suggested fix:** wait for transmitter-complete before de-asserting DE/RE (poll
  the UART TC flag, use the async API's TX-complete callback, or a computed,
  conservative delay), and make the delay baud-rate-derived.

## 26.2 Design / maintainability observations (no immediate bug)

### M7 — Supervisor reaches into subsystem internals without locks

`subsys/bms/bms_system.c` reads `sys->runtime->events` (`:21`),
`sys->measurement->running` (`:100`), `sys->runtime->running` (`:112`),
`sys->runtime->last_processed_sequence` (`:116`), and
`sys->communications->running` (`:139`). It works today, but it couples the
supervisor to internal struct layouts and skips the services' locks. Adding small
accessors (`..._is_running()`, an events accessor) would keep the layering honest.

### M8 — Duplicated config serialisation

The same field-by-field config encoding appears twice: `subsys/bms/bms_config_store.c`
(persistent image) and `subsys/bms/bms_protocol.c` (RS485 config payload). They must
be kept in lockstep by hand. Consider one shared serialiser plus a length-tagged
form, or at least a comment cross-referencing them.

### M9 — `bms_protocol_decode_cells_payload` does not zero its output

`subsys/bms/bms_protocol.c:462`. Fields other than `cell_count`/`cell_mv` are left
untouched, so a caller that does not pre-zero `out` may read uninitialised data.
Callers in this tree pre-zero; a `memset` inside the decoder would be safer.

### M10 — Startup ordering can produce a transient runtime error

`measurement_start` and `runtime_start` both schedule immediately; the runtime can
tick before the first snapshot is published, yielding one `-ENODATA` in
`last_error`. It clears on the next tick, but a supervisor that polled precisely in
that window could see a (momentary) critical flag. Starting measurement and
publishing once *before* starting runtime would remove the race.

### M11 — `process_once` is not re-entrancy protected

The shell command `bms runtime process` can run `process_once` concurrently with the
periodic work handler. Shared fields are mutex-protected, but two overlapping
actuations are theoretically possible. A dedicated "processing" flag/mutex would
make it single-flight.

### M12 — BQ bus efficiency

`bq76952_read_all_measurements` reads 16 cell registers one at a time, and
`bq76952_read_faults` reads 10 fault registers one at a time (~30 transactions per
AFE per cycle; more with two AFEs). The cell registers (`0x14..0x33`) and the
fault/status registers (`0x02..0x11`) are contiguous and could be read in a single
multi-byte transfer each, cutting bus load several-fold.

### M13 — Minor: uninitialised index edge and diagnostic races

`bms_monitor_bq76952.c` relies on `memset` to leave min/max index at 0, which is
correct for index 0 but easy to misread. Shell commands mutate
`runtime.applied_balance_mask` (`main_native.c:1361,1398`) without the runtime lock.
Both are low risk and diagnostic-only.

## 26.3 Things that are correct but worth verifying on hardware

* The BQ Safety Status A bit map used in `bq76952.c` matches the TI register map
  (CUV=bit2, COV=bit3, OCC=bit4, OCD1=bit5, OCD2=bit6, SCD=bit7) — verified against
  the TI reference manual. Good.
* `I2C_DT_SPEC_GET` + `ctx->i2c.addr << 1` for the CRC address byte assumes
  `i2c_dt_spec.addr` is the 7-bit address, which is Zephyr's convention. Verify with
  a logic analyser on first I2C bring-up.
* The pinctrl node names in the overlays (`can1_rx_pd0`, `uart4_tx_pc10`,
  `i2c2_scl_pf1`, …) must exist in the chosen STM32F4 pinctrl definitions; a target
  build is the only proof.
* ALERT polarity (`GPIO_ACTIVE_HIGH` in DTS) must match the BQ's configured ALERT
  polarity.

## 26.4 Suggested priority order

1. **M1** (wrong 32S pack voltage in simulation) — cheap, improves demo correctness,
   and removes a documentation-vs-code contradiction.
2. **M6** (RS485 timing) and **M5** (Data Memory programming) — the two biggest
   real-hardware readiness items.
3. **M3/M4** (temperature + geometry/scaling) — the next functional features.
4. **M2, M7–M13** — incremental robustness and cleanliness.

## 26.5 Change log — fixes applied in this revision

| ID | Fix | Files changed |
|---|---|---|
| **M1** | Native 32S pack voltage now uses the summed-stack fallback (provider = `NULL`), matching the STM32 32S profile and the documentation. Added a `t_topology` assertion that `pack_mv == stack_mv > 0`. | `src/main_native.c` |
| **M2** | `bms_core_runtime_force_safe_outputs` now **always attempts the FET all-off action**, even if clearing the balance mask fails; the balance-clear error is still reported. Previously a failed balance write returned early and skipped `ALL_OFF`. | `subsys/bms/bms_core_runtime.c` |
| **M6** | RS485 transmit now waits for UART completion (bounded, via `uart_irq_tx_complete`) before releasing DE/RE, instead of a fixed 120 µs delay. | `platform/stm32/bms_communications_stm32.c` |
| **M9** | `bms_protocol_decode_cells_payload` now zeroes its output view before filling, so callers cannot read uninitialised fields. | `subsys/bms/bms_protocol.c` |
| **M10** | Both entry points publish one measurement synchronously before starting the core runtime, removing the transient `-ENODATA` (and a theoretical momentary supervisor false-positive). | `src/main_native.c`, `src/main_stm32.c` |

Still open by design: **M3** (temperature/alerts), **M4** (AFE geometry/scaling),
**M5** (BQ Data Memory programming), **M7** (supervisor encapsulation), **M8**
(serialisation duplication), **M11** (runtime re-entrancy), **M12** (bus batching),
**M13** (minor diagnostics). None of the fixes above were compiled on hardware in this
environment (no host compiler/toolchain was available), so they still need a normal
Zephyr build, the host unit tests, and `bms test all` + `bms scenario all` to confirm.

---

# PART VI — REFERENCE

# Appendix A — Unit and sign conventions

| Quantity | Unit | Notes |
|---|---|---|
| Cell/stack/pack voltage | mV (`int32`) | |
| Current | mA (`int32`) | **+ charge, − discharge** |
| SOC / SOH | centi-percent (`uint16`) | `10000` = 100.00 % |
| Capacity (config) | mAh | |
| Charge counters | µAh (`int64`/`uint64`) | 1000× mAh |
| Energy | µWh (`uint64`) | |
| EFC | milli-cycles (`uint32`) | `1000` = 1 cycle |
| Time | ms (`int64`) | from `k_uptime_get()` |
| Balance/fault masks | bitmaps, bit 0 = cell 1 / flag 0 | |

# Appendix B — Generic fault bitmap

| Bit | Flag |
|---:|---|
| 0 | `BMS_FAULT_CELL_UNDERVOLTAGE` |
| 1 | `BMS_FAULT_CELL_OVERVOLTAGE` |
| 2 | `BMS_FAULT_OVERCURRENT_CHARGE` |
| 3 | `BMS_FAULT_OVERCURRENT_DISCHARGE` |
| 4 | `BMS_FAULT_SHORT_CIRCUIT_DISCHARGE` |
| 24 | `BMS_FAULT_PERMANENT_FAIL` |

Protection re-expresses these in the `BMS_PROT_HW_*` namespace before deciding.

# Appendix C — Default configuration (all development values)

| Parameter | Default |
|---|---:|
| Measurement period | 200 ms |
| Runtime period | 100 ms |
| Snapshot stale limit | 1000 ms |
| UV trip / release | 2800 / 3000 mV |
| OV trip / release | 3650 / 3500 mV |
| Charge OC trip / release | +30000 / +25000 mA |
| Discharge OC trip / release magnitude | 50000 / 40000 mA |
| Balancing enabled | yes |
| Balancing start voltage | 3400 mV |
| Balancing start / stop delta | 20 / 10 mV |
| Balancing max \|current\| | 5000 mA |
| Balancing max cells | 4 |
| Design capacity | 100000 mAh |
| Initial full capacity | 100000 mAh |
| Initial SOC | 50.00 % |
| Charge efficiency | 100.0 % |
| Max estimator integration gap | 1000 ms |

# Appendix D — Shell command reference

## Native simulator (full)

```text
Layer tests:
  bms test transport | driver | monitor | topology | measurement | runtime
  bms test balancing | estimator | persistence | communications | system | all
Behaviour:
  bms scenario all
State:
  bms snapshot | state | runtime status
  bms measurement status|refresh|start|stop|period <ms>
  bms protection status|config
  bms balancing status|config|enable|disable
  bms estimator status|config|set_soc <pct>|set_full_capacity <mAh>|reset_counters
  bms system status|selftest|check|recover
  bms system watchdog status | bms system watchdog inject_error <errno>
  bms events show|clear
Config/persistence:
  bms config show|status|defaults|apply|save|load|simulate_reboot|erase
  bms config set periods <meas_ms> <rt_ms>
  bms config set uv <trip_mV> <release_mV>
  bms config set ov <trip_mV> <release_mV>
  bms config set charge_oc <trip_mA> <release_mA>
  bms config set discharge_oc <trip_mA> <release_mA>
  bms config set capacity <design_mAh> <initial_full_mAh>
Comms:
  bms comm status
  bms comm can publish|dump
  bms comm rs485 request status|cells|config
  bms comm rs485 stage_uv <trip_mV> <release_mV> | apply | save | dump
Diagnostics:
  bms raw <0|1>
  bms inject cell <bq> <cell 1..16> <mV>
  bms inject current <bq> <mA>
  bms inject uv <bq> on|off
  bms balance <global_mask>
  bms switch release|all_off|charge_off|discharge_off
```

## STM32 bring-up shell (small)

```text
bms status          # pack snapshot + state + faults + balance mask
bms system          # supervisor mode / self-test / safe latch / flags
bms board           # ALERT pin levels + interrupt counts
bms bq_ping <0|1>   # read BQ device number (0x7695 expected)
bms config_save     # persist applied config + estimator state
bms events          # event history
```

# Appendix E — Glossary

| Term | Definition |
|---|---|
| AFE | Analog Front End — the chip measuring cells/driving FETs (BQ76952). |
| Actuation | Physically writing to FETs/balancing. Gated by config during bring-up. |
| Coulomb counting | Tracking charge as ∫ current dt. |
| CRC-32 | 32-bit integrity check used in the persistent image. |
| CRC16-CCITT-FALSE | 16-bit frame check used in RS485 (poly 0x1021, init 0xFFFF). |
| Devicetree / DTS | Data description of the board's hardware wiring. |
| DEGRADED | System mode: communication problem only; battery control healthy. |
| EFC | Equivalent Full Cycles. |
| Hysteresis | Trip at one threshold, release at another, to stop chatter. |
| IWDG | STM32 independent watchdog. |
| `k_work_delayable` | Zephyr self-rescheduling deferred work item. |
| MAINTENANCE | System mode during controlled reconfiguration. |
| Monitor | The generic hardware-neutral AFE instance. |
| Native sim | Zephyr's PC target for running firmware without hardware. |
| Overlay | DTS file adding board details. |
| Provider | The AFE explicitly chosen to supply current / pack voltage / switches. |
| SAFE_LATCHED | System mode: critical fault; outputs safe; recovery is explicit. |
| Snapshot | One coherent whole-pack measurement picture. |
| SOC / SOH | State of Charge / State of Health. |
| Stack voltage | Voltage across one AFE's cells. |
| Stale | Snapshot older than the allowed age (fail-safe). |
| Topology | Mapping AFE segments to one logical pack. |
| Transfer buffer | The BQ 0x40..0x5F mailbox used for subcommands/Data Memory. |

# Appendix F — Explain-it-out-loud cheat sheet

**The 60-second pitch.**
> "STARZ BMS Zephyr v1.0 is a reusable battery-management firmware platform built on
> Zephyr. It supports three products from one source tree: a 32S virtual simulator
> that runs on a PC, an STM32F446 16S board with one TI BQ76952, and an STM32F446 32S
> board with two stacked BQ76952s. The core idea is a hard layer boundary: below the
> generic `bms_monitor` API everything is hardware-specific; above it everything is
> battery-domain logic that never touches a register. Measurement publishes one
> coherent snapshot, and protection, balancing, the estimator, and CAN/RS485 all
> consume the same snapshot. Because hardware sits behind that boundary, the entire
> stack runs under Zephyr's `native_sim`, which is how it is tested without a board.
> Physical FET actuation is deliberately disabled on the STM32 profiles until the
> board is validated."

**Questions you should be able to answer**

* *Why a snapshot instead of reading the AFE on demand?* — One coherent picture; no
  time skew between protection, comms, and estimation; consumers can detect new data
  by sequence number.
* *Why is the simulator valuable?* — Only the bottom transport is faked. The driver,
  adapter, topology, measurement, protection, state machine, balancing, estimator,
  config, protocols, and supervisor are the real code, so `bms test all` /
  `bms scenario all` are genuine regression tests.
* *Why two I2C buses for 32S?* — Both BQ76952s power up at 0x08; using separate MCU
  I2C controllers avoids irreversible OTP address programming.
* *How do you stop protection chatter?* — Separate trip/release thresholds with
  stateful hysteresis per reason.
* *What does `RELEASE` do?* — Releases host-forced inhibition; it does not force the
  FETs on against the BQ's own protection.
* *Why is current not summed across stacked AFEs?* — The same current flows through
  the whole series string; BQ0 is the single current provider.
* *Why is 32S pack voltage the sum of both stacks?* — No single stacked AFE sees the
  full pack, so topology sums the local stack voltages (provider = NULL).
* *What happens on a measurement or runtime failure?* — The supervisor latches
  `SAFE_LATCHED`, stops the runtime, clears balancing, forces `ALL_OFF`, and stops
  feeding the watchdog. Recovery is explicit.
* *What is not done?* — Temperature channels, automatic SOH learning, variable cell
  count, full BQ Data Memory programming, NVS by default, product CAN profiles, and
  all physical/electrical validation. Say this confidently; it shows engineering
  honesty.
* *How would you port to ESP32?* — Replace the platform backends (I2C, CAN/TWAI,
  UART, flash, watchdog) and the board description; leave the BMS core untouched.

**Strength you can point at:** the project favours *fail loudly at startup over
mystery at runtime* (topology and config validation), *fail safe on uncertainty*
(validity flags; stale snapshots block both directions), and *keep hardware
replaceable* (transport/monitor/platform boundaries).

---

# Appendix G — Where to look when you are stuck

| You want to understand… | Read |
|---|---|
| The generic AFE contract | `include/bms/bms_monitor.h` |
| The whole-pack picture | `include/bms/bms_snapshot.h` |
| Protection policy | `subsys/bms/bms_protection.c` |
| The decision loop | `subsys/bms/bms_core_runtime.c` |
| SOC math | `subsys/bms/bms_estimator.c` |
| Config validation | `subsys/bms/bms_config.c` |
| The persistent format | `subsys/bms/bms_config_store.c` |
| CAN/RS485 bytes | `subsys/bms/bms_protocol.c` |
| Health/safe-latch | `subsys/bms/bms_system.c`, `bms_system_policy.c` |
| Real I2C/BQ mailbox | `drivers/.../bq76952_transport_i2c.c` |
| 32S wiring | `overlays/stm32f446_32s.overlay`, `docs/HARDWARE_32S.md` |
| Bring-up order | `docs/BUILD_MATRIX.md`, Chapter 23 above |

*End of the book. The source tree remains the final authority.*

