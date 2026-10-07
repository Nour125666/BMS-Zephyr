# ⚡ BMS on Zephyr RTOS

**One BMS brain. Different cell counts. Different boards. Same core.**

![Zephyr RTOS](https://img.shields.io/badge/RTOS-Zephyr-6C63FF?style=flat-square)
![MCU](https://img.shields.io/badge/MCU-STM32F446-03234B?style=flat-square)
![AFE](https://img.shields.io/badge/AFE-BQ76952-EA4335?style=flat-square)
![Pack](https://img.shields.io/badge/Pack-1S%E2%86%9232S-2E8B57?style=flat-square)
![Simulation](https://img.shields.io/badge/native__sim-tested-2E8B57?style=flat-square)
![Hardware](https://img.shields.io/badge/hardware-validation%20pending-F0A500?style=flat-square)

> [!NOTE]
> The software side can already run as a complete virtual BMS.  
> The physical STM32/BQ/FET/isolated-32S side still has to earn its green checkmarks on the bench.

### 🧭 Pick your route

| I want to... | Start here |
|---|---|
| 🧪 Run the whole BMS without hardware | `native_sim` |
| 🔋 Build a <=16S STM32 pack | `config/stm32_16s.conf` |
| ⚡ Build a <=32S stacked-AFE pack | `config/stm32_32s.conf` |
| 🧠 Understand why the architecture looks like this | **Why Zephyr?** + **Topology** |
| 🔌 Bring up real hardware | **Recommended bring-up path** |



This repository is a battery-management firmware project, but the part that makes it interesting is really **Zephyr RTOS**.

The original problem looked simple enough:

```text
STM32 + BQ76952 + some cells = BMS
```

Then the requirements started moving.

```text
16 cells
   ↓
maybe 19 cells
   ↓
maybe 20 cells
   ↓
32 cells
   ↓
one BQ becomes two BQs
   ↓
one I2C domain becomes two electrical domains
   ↓
maybe STM32 today, ESP32 later
```

At that point, firmware that is written directly around one CubeMX project, one MCU peripheral instance, and one fixed cell count gets painful very quickly.

So the goal of this project became:

> **Let the hardware change without forcing the BMS logic to be rewritten every time.**

That is why Zephyr was chosen.

Not because an RTOS is fashionable, and not because a BMS needs hundreds of threads.  
Zephyr is valuable here because it gives us a clean way to separate:

```text
what the BMS does
        from
what hardware happens to be underneath it
```

The result is a firmware platform that currently supports:

```text
native_sim
   └── 32S virtual pack, two virtual BQ76952 devices

STM32F446 / <=16S
   └── one physical BQ76952

STM32F446 / <=32S
   └── two stacked BQ76952 devices
```

and is intentionally structured so an ESP32 board layer can be added later without rewriting protection, balancing, estimation, topology, or communications logic.

---

## 🧷 Quick links

- [🌀 Why Zephyr?](#-why-zephyr-for-a-bms)
- [🗺️ Architecture](#️-the-project-in-one-diagram)
- [🧩 Topology](#-why-the-topology-layer-matters)
- [🎯 Targets](#-supported-targets)
- [🏗️ Build profiles](#️-build-profiles)
- [🎮 Shell commands](#-useful-shell-commands)
- [✅ What is tested](#-what-has-actually-been-tested)
- [🔌 What still needs hardware](#-where-the-real-world-still-gets-a-vote)
- [🪜 Bring-up path](#-recommended-bring-up-path)

---

## 🌀 Why Zephyr for a BMS?

This is the most important design decision in the repository.

A traditional MCU project often ends up looking like this:

```text
main.c
├── HAL_I2C_Mem_Read(...)
├── HAL_GPIO_WritePin(...)
├── HAL_CAN_AddTxMessage(...)
├── HAL_UART_Transmit(...)
├── cell_count == 16 ?
├── cell_count == 32 ?
├── bq0 ?
├── bq1 ?
└── a growing collection of hardware assumptions
```

That can work perfectly well for one board.

Then the board changes. Of course it does.

For this project, the hardware already changes significantly between <=16S and >16S:

```text
<=16S
  STM32
    ↓
  one BQ76952
    ↓
  cells 1..16


>16S / <=32S
  STM32
   ├── bottom BQ76952 → cells 1..16
   └── top BQ76952    → cells 17..32
             ↑
       isolated domain
```

A 32S board is not just a 16S board with a larger array.

It changes:

- the number of AFEs;
- the pack topology;
- the I2C routing;
- the electrical isolation boundary;
- which AFE provides current;
- which AFE controls the power stage;
- how balancing masks are split;
- how faults are aggregated;
- how total pack voltage is calculated.

Zephyr gives us a natural place for those differences to live.

```text
              SAME APPLICATION / BMS CORE
                         │
                         │
              +----------+----------+
              |                     |
              v                     v
        16S board profile      32S board profile
              |                     |
          Devicetree             Devicetree
          pinctrl                pinctrl
          Kconfig                Kconfig
              |                     |
              v                     v
        one monitor            two monitors
```

The upper layers do not need to care.

That is the point.

---

## 🧰 What Zephyr is doing for us

Zephyr is not just the scheduler in this project.

It provides the **hardware abstraction strategy**.

### 🌳 Devicetree: describe the board, do not hard-code the board

Instead of spreading pin numbers through C code:

```c
GPIOB
GPIO_PIN_14
I2C1
USART4
```

the STM32 hardware is described in Devicetree and overlays.

Conceptually:

```text
Devicetree says:
    "there is a BQ76952 on this I2C controller"
    "this GPIO is ALERT"
    "this UART is RS485"
    "this CAN controller is enabled"

pinctrl says:
    "I2C1 SCL is PB8"
    "I2C1 SDA is PB9"
    "CAN RX/TX are PD0/PD1"
```

The C code asks Zephyr for the device.

It does not need to know which pin STM32CubeMX once assigned.

### 🎛️ Kconfig: describe the software build

The same source tree can build different products by changing configuration rather than forking the code.

```text
config/native_sim.conf
config/stm32_16s.conf
config/stm32_32s.conf
```

This lets the build choose things such as:

- virtual or real hardware;
- one or two BQ monitors;
- maximum logical cell count;
- physical actuation enabled or disabled;
- persistence backend;
- communication backend.

### 🔌 Zephyr drivers: stop reinventing MCU plumbing

The project can use the same API style for:

```text
I2C
CAN
UART
GPIO
watchdog
flash / NVS
```

The STM32 implementation lives below Zephyr's device model.

A future ESP32 port should therefore replace the board/platform layer, not the whole BMS.

### 🧪 `native_sim`: the BMS can run before the PCB exists

This is one of the most useful parts of the project — basically a BMS bench that fits inside a terminal.

The complete BMS control path can run as a native Zephyr application:

```text
virtual BQ
   ↓
monitor
   ↓
topology
   ↓
measurement
   ↓
snapshot
   ↓
protection / balancing / estimator
   ↓
state machine
   ↓
virtual outputs
```

That means a large amount of logic can be tested without:

- a battery pack;
- an STM32;
- a CAN transceiver;
- MOSFETs;
- an isolated I2C link;
- or a lab bench.

The oscilloscope still gets the final vote, but it does not need to be invited to debug an array-indexing bug in Cell 17.

---

# 🗺️ The project in one diagram

```text
                           ┌──────────────────────────────┐
                           │        Zephyr RTOS           │
                           │                              │
                           │ Devicetree • Kconfig         │
                           │ device model • work queues   │
                           │ shell • I2C • CAN • UART     │
                           │ GPIO • watchdog • NVS        │
                           └──────────────┬───────────────┘
                                          │
                                          │ hardware boundary
                                          ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         BMS PLATFORM                          │
│                                                                     │
│  monitor(s) → topology → measurement → published snapshot          │
│                                  │                                  │
│                 ┌────────────────┼────────────────┐                 │
│                 ▼                ▼                ▼                 │
│            protection        balancing        estimator             │
│                 │                                 │                 │
│                 ▼                                 ▼                 │
│           state machine                       SOC / energy           │
│                 │                                                   │
│                 ▼                                                   │
│              outputs                                                │
│                                                                     │
│      config / persistence    CAN / RS485    system supervisor        │
└─────────────────────────────────────────────────────────────────────┘
                                          │
                 ┌────────────────────────┼────────────────────────┐
                 │                        │                        │
                 ▼                        ▼                        ▼
             native_sim              STM32 16S                STM32 32S
          2 virtual AFEs           1 physical AFE           2 physical AFEs
```

---

# 😵 The problem this architecture is trying to avoid

Imagine protection code written directly around two BQs:

```c
if (cell < 16) {
    voltage = bq0_read_cell(cell);
} else {
    voltage = bq1_read_cell(cell - 16);
}
```

Then balancing:

```c
if (cell < 16) {
    ...
} else {
    ...
}
```

Then CAN.

Then SOC.

Then diagnostics.

Then a new 20S product appears.

Then an ESP32 product appears.

Soon the BMS is less of a control system and more of an archaeological site made of `if` statements.

This project instead creates one logical battery.

```text
physical hardware                    logical pack

BQ0 local 1  ──────────────────────→ Cell 1
BQ0 local 2  ──────────────────────→ Cell 2
...
BQ0 local 16 ──────────────────────→ Cell 16

BQ1 local 1  ──────────────────────→ Cell 17
BQ1 local 2  ──────────────────────→ Cell 18
...
BQ1 local 16 ──────────────────────→ Cell 32
```

Everything above topology sees:

```text
Cell 1 ... Cell N
```

not:

```text
BQ0 register this
BQ1 register that
```

That is what makes changing the number of cells manageable.

---

# 🎯 Supported targets

| Build | MCU | Battery monitor arrangement | Logical cells | Main purpose |
|---|---|---|---:|---|
| `native_sim` | Host PC | 2 virtual BQ76952 | 32 | Development and regression testing |
| STM32 16S | STM32F446ZE | 1 x BQ76952 | <=16 | Single-AFE physical target |
| STM32 32S | STM32F446ZE | 2 x stacked BQ76952 | <=32 | Dual-AFE physical target |

The exact pack can use fewer cells than the maximum supported by the hardware profile. The important architectural distinction is whether the product uses one monitor segment or multiple monitor segments.

---

# 🔋 16S and 32S are different boards, not different BMS applications

This distinction is central to the repository.

## <=16S

```text
STM32F446ZE
     │
     │ I2C1
     │ PB8 / PB9
     ▼
 BQ76952
     │
     └── Cells 1..16
```

The monitor is both a physical AFE and the only logical segment of the pack.

## <=32S

```text
                         STM32F446ZE
                         /          \
                        /            \
                 I2C1 direct      I2C2 isolated
                    │                  │
                    ▼                  ▼
              BQ0 bottom          ISO1640
              Cells 1..16             │
                                      ▼
                                  BQ1 top
                                  Cells 17..32
```

The top AFE is in a different electrical domain.

The firmware still creates one logical pack:

```text
[ BQ0 cells 1..16 ] + [ BQ1 cells 1..16 ]
                   ↓
               Pack 1..32
```

The 32S reference profile deliberately uses two separate STM32 I2C controllers, which allows both BQ76952 devices to use the normal 7-bit address `0x08`.

No OTP address change is required by this reference software architecture.

---

# 🧩 Why the topology layer matters

Topology is the bridge between hardware reality and battery reality.

It answers questions such as:

```text
Which monitor owns Cell 23?
Which monitor provides current?
Where should global balance bit 19 go?
Which monitor controls the FETs?
Should faults be aggregated?
How is total pack voltage produced?
```

For 16S, topology is simple:

```text
monitor0 → Cells 1..16
current  → monitor0
switches → monitor0
```

For 32S, roles can be asymmetric:

```text
monitor0 → Cells 1..16
monitor1 → Cells 17..32

current provider       → monitor0
switch provider        → monitor1
faults                 → aggregate monitor0 + monitor1
balancing              → split global mask between both monitors
pack voltage           → stack0 + stack1
```

That asymmetry matters.

Two stacked AFEs do **not** mean every measurement should be added together.

For example:

```text
current0 = pack current
current1 = not another current to add to current0
```

The BMS chooses an explicit provider.

This is a good example of why topology belongs in its own layer.

---

# 🧱 The software stack

A useful way to read the code is from the hardware upward.

```text
transport
   ↓
BQ76952 semantic driver
   ↓
generic monitor
   ↓
topology
   ↓
measurement
   ↓
snapshot
   ↓
services
```

## 1. Transport — "how do bytes move?"

The transport layer deals with things such as:

```text
I2C write
I2C read
subcommand transfer
Data Memory transfer
virtual register model
```

It should not decide whether a voltage is safe.

On native simulation, transport talks to a virtual BQ model.

On STM32, transport talks to Zephyr I2C.

```text
same semantic BQ driver
        │
        ├── virtual transport
        │
        └── Zephyr I2C transport
```

This is the first major hardware boundary.

---

## 2. BQ76952 semantic driver — "what do those bytes mean?"

The BQ driver understands TI concepts:

```text
Cell1Voltage
StackVoltage
PackVoltage
CC2 current
SafetyAlert
SafetyStatus
PFStatus
FET status
subcommands
Data Memory
```

It converts raw register transactions into meaningful AFE operations.

It is still hardware-specific, but it is no longer MCU-specific.

That difference is important.

```text
STM32-specific?  no
BQ76952-specific? yes
```

---

## 3. Generic monitor — "what can a battery monitor do?"

Above the BQ driver, the rest of the BMS sees a generic monitor API.

Typical capabilities include:

```text
CELL_VOLTAGE
STACK_VOLTAGE
PACK_VOLTAGE
CURRENT
TEMPERATURE
LOCAL_PROTECTION
BALANCING
SWITCH_CONTROL
POWER_STATE
CHARGE_COUNTER
```

The monitor interface is the line where the upper BMS stops caring that the current AFE happens to be a BQ76952.

A future AFE could implement the same monitor contract.

---

## 4. Measurement — "take one picture of the pack"

A BMS has many consumers of data:

```text
protection
balancing
SOC
CAN
RS485
diagnostics
```

If every one of them independently reads hardware, they can all see slightly different moments in time.

Instead, measurement publishes one coherent snapshot.

```text
hardware acquisition
        ↓
Snapshot #100
        ↓
 ┌──────┼────────┬───────────┐
 ▼      ▼        ▼           ▼
protect balance estimator communications
```

Then:

```text
Snapshot #101
Snapshot #102
...
```

This is one of the simplest architectural decisions in the project, and one of the most useful. Everybody gets the same photo; nobody sneaks off to the I2C bus for their own private version of reality.

---

# 📸 A snapshot is the common language of the BMS

A snapshot contains the logical state of the pack at one observation point.

Examples include:

```text
cell voltages
cell minimum / maximum
stack voltage
pack voltage
current
faults
balance state
switch state
timestamp
sequence number
```

The snapshot is not a database and it is not flash storage.

It is the latest coherent published observation.

That means CAN and protection can talk about the same battery state rather than racing each other to the I2C bus.

---

# 🛡️ Protection

Protection consumes the snapshot and produces a decision.

```text
snapshot
   ↓
protection policy
   ↓
charge_allowed
discharge_allowed
reasons
```

The policy currently covers the software-side path for:

```text
cell undervoltage
cell overvoltage
charge overcurrent
discharge overcurrent
stale measurement
hardware-derived AFE faults
permanent-failure conditions
```

Protection has hysteresis.

That means a threshold crossing can trip protection without immediately releasing again because the measurement moved by 1 mV.

The development thresholds in the repository are **simulation/development values**, not certified product limits.

Do not treat them as chemistry-specific production settings.

---

# 🚦 State machine — one place gets to decide what the BMS does

Protection answers:

```text
"what is allowed?"
```

The state machine answers:

```text
"what should I command?"
```

States include:

```text
INIT
NORMAL
CHARGE_BLOCKED
DISCHARGE_BLOCKED
ALL_BLOCKED
```

The mapping is intentionally boring:

```text
CHG allowed + DSG allowed
        ↓
      NORMAL

CHG blocked + DSG allowed
        ↓
  CHARGE_BLOCKED

CHG allowed + DSG blocked
        ↓
 DISCHARGE_BLOCKED

both blocked
        ↓
    ALL_BLOCKED
```

Boring is excellent here.

Safety logic should be easy to explain at 2 AM with a scope probe in one hand and cold coffee in the other.

---

# ⚖️ Balancing

Balancing is another snapshot consumer.

It decides **which logical pack cells should balance**, not which BQ register bit to write.

Example:

```text
balance Cell 1 + Cell 17
        ↓
global balance mask
        ↓
topology
       / \
      /   \
 BQ0 bit1  BQ1 bit1
```

So the balancing policy works for a 16S or 32S logical pack without knowing how many AFEs exist underneath it.

That is exactly the sort of thing this architecture is built for.

The current balancing thresholds are development settings and still require physical validation.

---

# 🔢 SOC, energy, and SOH

The estimator consumes published current snapshots.

Current convention:

```text
+ current = charging
- current = discharging
```

SOC is presently based on coulomb counting.

The estimator also tracks:

```text
charged charge
discharged charge
throughput
energy in
energy out
equivalent full cycles
```

Duplicate snapshot sequence numbers are ignored, because the core runtime may execute faster than measurement publication.

Unexpectedly long data gaps are not integrated blindly.

## About SOH

The project does **not** pretend to have a finished automatic SOH-learning algorithm.

Current capacity-based SOH is:

```text
estimated usable full capacity
------------------------------
design capacity
```

The architecture for storing and reporting it exists.

A real learned-capacity algorithm still needs validated full/empty anchors, cycling rules, persistence strategy, and physical battery data.

---

# 💾 Configuration and persistence

Configuration is separated from source code.

It includes things such as:

```text
measurement period
runtime period
UV / OV thresholds
overcurrent thresholds
stale-data timeout
balancing policy
estimator capacity parameters
```

Configuration changes use a staged model:

```text
edit
  ↓
STAGED CONFIG
  ↓
validate
  ↓
apply
  ↓
APPLIED CONFIG
```

Why bother?

Because this is much safer than changing one protection value immediately while five related values are still being edited.

Persistence uses a versioned binary format with:

```text
magic
format version
payload length
generation
CRC-32
serialized configuration
retained estimator state
```

The native simulator uses a virtual backend.

The STM32 profile includes an NVS backend path, but final flash partitioning must be approved before it should be considered production-ready.

---

# 📡 CAN and RS485

Communications sit above the BMS core.

They do not read the BQ directly.

```text
snapshot + protection + estimator + state
                    ↓
             telemetry view
                /      \
               /        \
             CAN       RS485
```

## CAN

The repository contains a project reference CAN telemetry profile.

It carries information such as:

```text
pack voltage
pack current
SOC
state
permissions
min/max cell
faults
balance mask
SOH / capacity
cell voltages
```

The reference IDs are not advertised as Pylontech, Victron, J1939, CANopen, or another vendor protocol.

Those can be added as product-specific protocol adapters later.

## RS485

The reference RS485 protocol uses framed messages:

```text
SOF
version
message type
sequence
payload length
payload
CRC16
```

It supports:

```text
status
cell values
configuration read
staged configuration write
apply
save
```

Remote configuration writes are useful for simulation and engineering, but should not simply be exposed in a production product without an authorization strategy.

---

# 👀 The system supervisor: the part watching the parts

The BMS core has a system supervisor that watches for software-health problems.

Examples:

```text
no snapshot
stale measurement
measurement stopped
measurement not progressing
runtime stopped
runtime not progressing
communication errors
communication not progressing
startup self-test failure
```

Not every problem is treated the same.

```text
CAN/RS485 problem
       ↓
    DEGRADED
       ↓
battery protection can keep running
```

but:

```text
measurement/runtime problem
          ↓
      SAFE_LATCHED
          ↓
clear balancing
force safe outputs
stop normal actuation loop
```

Recovery is explicit.

The system does not quietly re-enable the power stage because one good measurement appeared after a serious failure.

---

# ⏱️ Watchdog

The supervisor owns the decision to feed the watchdog.

That matters because:

```text
"the CPU is alive"
```

is not the same thing as:

```text
"the BMS control loop is healthy"
```

In native simulation the project uses a virtual watchdog backend.

The STM32 layer provides the path to a hardware watchdog backend.

---

# 💬 Zephyr shell: the firmware has a conversation mode

During development, the shell is one of the nicest parts of the project.

Instead of recompiling just to inspect one state:

```text
uart:~$ bms snapshot
uart:~$ bms protection status
uart:~$ bms state
uart:~$ bms estimator status
uart:~$ bms system status
```

Virtual hardware can also be changed live:

```text
uart:~$ bms inject cell 1 1 2500
uart:~$ bms inject current 0 -60000
uart:~$ bms inject uv 1 on
```

This makes `native_sim` behave less like a unit test and more like a little virtual BMS bench.

---

# 🗂️ Repository layout

```text
.
├── CMakeLists.txt
├── Kconfig
├── prj.conf
│
├── config/
│   ├── native_sim.conf
│   ├── stm32_16s.conf
│   └── stm32_32s.conf
│
├── overlays/
│   ├── stm32f446_16s.overlay
│   ├── stm32f446_32s.overlay
│   └── stm32f446_storage_nvs.overlay
│
├── include/
│   ├── bms/
│   │   ├── bms_monitor.h
│   │   ├── bms_topology.h
│   │   ├── bms_snapshot.h
│   │   ├── bms_measurement.h
│   │   ├── bms_protection.h
│   │   ├── bms_balancing.h
│   │   ├── bms_estimator.h
│   │   ├── bms_state_machine.h
│   │   ├── bms_core_runtime.h
│   │   ├── bms_config.h
│   │   ├── bms_config_store.h
│   │   ├── bms_protocol.h
│   │   ├── bms_communications.h
│   │   ├── bms_system.h
│   │   └── ...
│   │
│   └── bq76952/
│
├── subsys/
│   └── bms/
│       ├── bms_monitor.c
│       ├── bms_topology.c
│       ├── bms_measurement.c
│       ├── bms_protection.c
│       ├── bms_balancing.c
│       ├── bms_estimator.c
│       ├── bms_state_machine.c
│       ├── bms_core_runtime.c
│       ├── bms_config.c
│       ├── bms_config_store.c
│       ├── bms_protocol.c
│       ├── bms_communications.c
│       ├── bms_system.c
│       └── virtual backends
│
├── drivers/
│   └── bq76952/
│
├── platform/
│   └── stm32/
│       ├── board integration
│       ├── communication backends
│       ├── watchdog backend
│       └── NVS backend
│
├── src/
│   ├── main_native.c
│   └── main_stm32.c
│
├── docs/
│   ├── HARDWARE_16S.md
│   ├── HARDWARE_32S.md
│   ├── BUILD_MATRIX.md
│   ├── SOURCE_BASIS.md
│   ├── DEEPSEEK_PROJECT_CONTEXT.md
│   └── BMS_Zephyr_v1.0_Technical_Manual.*
│
└── reference/
    └── VS_BMS_16S_reference.ioc
```

A good way to browse the repository is:

```text
start with README
      ↓
bms_monitor.h
      ↓
bms_topology.h/.c
      ↓
bms_measurement.c
      ↓
bms_protection.c
      ↓
bms_state_machine.c
      ↓
main_native.c
      ↓
STM32 overlays / platform layer
```

---

# 🏗️ Build profiles

## 1. Native simulator

```bash
west build -p always -b native_sim/native/64 . -- \
  -DCONF_FILE=config/native_sim.conf
```

Find and run the executable:

```bash
find build -name zephyr.exe -type f -print
```

Run the returned path.

`native_sim` normally prints a pseudo-terminal such as:

```text
uart connected to pseudotty: /dev/pts/7
```

Connect from a second terminal:

```bash
screen /dev/pts/7
```

---

## 2. STM32F446 <=16S

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_16s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_16s.overlay
```

This profile represents:

```text
STM32F446ZE
    ↓
I2C1 PB8/PB9
    ↓
BQ76952 @ 0x08
    ↓
up to 16 cells
```

---

## 3. STM32F446 <=32S

```bash
west build -p always -b nucleo_f446ze . -- \
  -DCONF_FILE=config/stm32_32s.conf \
  -DDTC_OVERLAY_FILE=overlays/stm32f446_32s.overlay
```

Reference architecture:

```text
STM32F446ZE
├── I2C1 → lower BQ76952
└── I2C2 → ISO1640 → upper BQ76952
```

The 32S upper GPIO routing in this repository is a **reference hardware definition**, not a claim that a finished PCB already exists with those exact routes.

---

# 🎮 Useful shell commands

## Start here

```text
bms test all
bms scenario all
bms snapshot
bms system status
```

## Measurement

```text
bms measurement status
bms measurement refresh
bms measurement start
bms measurement stop
bms measurement period <ms>
```

## 🛡️ Protection / state

```text
bms protection status
bms protection config
bms state
bms events show
bms events clear
```

## Estimation

```text
bms estimator status
bms estimator config
bms estimator set_soc <0..100>
bms estimator set_full_capacity <mAh>
bms estimator reset_counters
```

## Configuration

```text
bms config show
bms config status
bms config defaults
bms config apply
bms config save
bms config load
bms config erase
bms config simulate_reboot
```

## Communications

```text
bms comm status
bms comm can publish
bms comm can dump
bms comm rs485 request status
bms comm rs485 request cells
bms comm rs485 request config
bms comm rs485 dump
```

## Virtual fault injection

```text
bms inject cell <bq> <cell> <mV>
bms inject current <bq> <mA>
bms inject uv <bq> on|off
```

---

# 🧪 A quick virtual demo

Start from a healthy pack:

```text
bms test all
bms snapshot
bms protection status
bms state
```

Expected idea:

```text
state = NORMAL
charge allowed = yes
discharge allowed = yes
```

Now make BQ1 local Cell 1 low:

```text
bms inject cell 1 1 2500
```

Because BQ1 is the second 16-cell segment:

```text
BQ1 local Cell 1
       ↓
logical Pack Cell 17
```

The rest of the BMS should see something similar to:

```text
minimum cell = Cell 17 / 2500 mV
SW_CELL_UV
charge allowed = yes
discharge allowed = no
state = DISCHARGE_BLOCKED
```

Notice what did **not** happen:

Protection did not contain:

```c
if (bq_index == 1 && local_cell == 1)
```

It simply saw:

```text
Cell 17 = 2500 mV
```

That is the architecture doing its job.

---

# ✅ What has actually been tested?

This section is intentionally conservative.

A BMS project can look very convincing in a terminal while still being completely wrong on a real power board.

So the repository distinguishes software validation from hardware validation.

## Software / virtual validation

The following areas have been exercised in host tests and/or the Zephyr native simulation during development:

**Legend:** ✅ exercised successfully · 🧪 host/native simulation · 🔌 still needs physical hardware

| Area | Status |
|---|---|
| Virtual BQ transport contract | ✅ Tested |
| BQ semantic-driver behavior in virtual stack | ✅ Tested |
| Generic monitor abstraction | ✅ Tested |
| 16-to-32-cell topology mapping | 🧪 Tested virtually |
| Snapshot generation | 🧪 Tested virtually |
| Cached measurement publication | 🧪 Tested virtually |
| Software UV/OV decisions | ✅ Tested |
| Charge/discharge overcurrent policy | ✅ Tested |
| Protection hysteresis | ✅ Tested |
| Stale-data fail-safe logic | ✅ Tested |
| State-machine transitions | 🧪 Tested virtually |
| Global balancing policy | 🧪 Tested virtually |
| SOC coulomb counting | 🧪 Host tested |
| Charge / energy counters | 🧪 Host tested |
| Capacity-based SOH plumbing | 🧪 Host tested |
| Configuration validation | 🧪 Host tested |
| CRC/versioned persistence format | 🧪 Host tested |
| CAN protocol encoding | 🧪 Host tested |
| RS485 framing / CRC / decoding | 🧪 Host tested |
| System-health classification | 🧪 Host tested |
| SAFE / degraded policy logic | 🧪 Host tested |
| Full virtual fault scenarios | ✅ Exercised during development |

Recorded validation includes successful paths such as:

```text
[PASS] transport-contract
[PASS] semantic-driver
[PASS] generic-monitor
[PASS] topology + snapshot
[PASS] measurement + cached snapshot publication
[PASS] protection decision
[PASS] state machine
[PASS] FULL VIRTUAL STACK + CORE RUNTIME
```

Later host-test work also validated the estimator, configuration store, communication protocol, and system-health policy independently with strict compiler warnings.

---

# 🔌 Where the real world still gets a vote

This is the part that matters before anyone calls the project "production-ready".

## The STM32 hardware still needs to meet the software

The generic architecture is far ahead of the physical bring-up.

The following items still require a real board, instruments, and controlled testing.

### Real BQ76952 I2C

Still to verify physically:

```text
ACK behavior
bus pull-ups
rise/fall time
real subcommand timing
Data Memory timing
CRC mode if used
error recovery
SCL/SDA behavior during BQ power-state changes
```

### ALERT

Still to verify:

```text
actual polarity
electrical configuration
interrupt edge
clear behavior
latency
behavior during multiple simultaneous faults
```

### RST_SHUT / DFETOFF

The MCU-side definitions exist, but the actual board-level electrical behavior must be proven.

Especially:

```text
RST_SHUT reset timing
shutdown timing
wake behavior
DFETOFF polarity
whether DFETOFF is physically implemented on the final board
```

### FET control

This is the big one.

The simulator can say:

```text
DSG = OFF
```

but only the real gate-driver / MOSFET stage can prove:

```text
DSG is actually off.
```

Physical validation must include:

```text
CHG off
DSG off
all off
release
body-diode behavior
precharge / predischarge if used
fault turn-off timing
gate voltage
switching transients
```

For this reason, the STM32 hardware profiles default toward conservative actuation during first bring-up.

### ⚖️ Balancing

Software can select the correct logical cells.

Hardware still needs to prove:

```text
the right physical cell balances
balance current is correct
temperature remains acceptable
adjacent-cell restrictions are respected if required
measurement is not corrupted
```

### Current calibration

Sadly, no amount of clean architecture can meditate its way into discovering the real shunt value.

The board still needs:

```text
shunt resistance confirmation
gain calibration
offset calibration
charge-current accuracy
discharge-current accuracy
zero-current drift
temperature influence
```

### Cell-voltage accuracy

Every cell input must eventually be checked against calibrated equipment.

### Temperature sensing

NTC wiring, biasing, conversion model, mechanical placement, and fault behavior need physical verification.

### CAN physical layer

The CAN codec can be correct while the bus still does nothing.

Need to verify:

```text
transceiver
termination
bit timing
dominant/recessive levels
bus-off recovery
EMC behavior
real inverter compatibility
```

### RS485 physical layer

Need to verify:

```text
transceiver
DE/RE polarity
turnaround timing
termination/biasing
multi-node behavior
noise immunity
```

### STM32 NVS

The persistence format is tested.

Real flash behavior still needs:

```text
partition confirmation
erase/write cycles
power-loss behavior
corruption recovery
wear considerations
```

### Hardware watchdog

The policy exists.

The real watchdog still needs to prove that a frozen firmware actually causes the expected reset/recovery behavior.

### 32S isolation

The 32S design adds the most important electrical boundary in the project.

The upper AFE floats above the lower 16-cell stack, so the final hardware needs careful validation of:

```text
ISO1640 power domains
isolation rating
upper-side pull-ups
power sequencing
top-AFE ALERT translation
RST_SHUT translation
DFETOFF / CFETOFF path
creepage
clearance
fault behavior
```

That part cannot be validated by `native_sim`, no matter how many green `[PASS]` messages it prints.

---

# 🪜 Recommended bring-up path

Do not jump directly from native simulation to a fully energized 32S battery.

A calmer path is much more useful.

```text
1. STM32 only
      ↓
2. STM32 + one powered BQ
      ↓
3. controlled cell inputs
      ↓
4. current calibration
      ↓
5. FET control
      ↓
6. balancing
      ↓
7. CAN / RS485
      ↓
8. second AFE + isolation
      ↓
9. full 32S stack
      ↓
10. thermal / abuse / EMC / qualification
```

## Stage 1 — STM32 only

Prove:

```text
Zephyr boots
console works
Devicetree resolves
GPIO devices are ready
CAN/UART/I2C controllers initialize
watchdog can be configured
```

## Stage 2 — STM32 + BQ, no power stage

Prove:

```text
Device Number read
cell register reads
stack voltage reads
fault/status reads
ALERT
RST_SHUT
```

## Stage 3 — controlled cells

Use a safe test setup to verify cell mapping and voltage accuracy.

Especially verify the transition:

```text
local Cell 16
        ↓
logical Cell 16

second AFE local Cell 1
        ↓
logical Cell 17
```

That boundary is worth testing very deliberately.

## Stage 4 — current path

Calibrate current before letting SOC calculations become trusted data.

## Stage 5 — FETs

Only after communication and measurement are stable should hardware actuation be enabled.

## Stage 6 — balancing

Verify one cell at a time before allowing policy-driven multi-cell balancing.

## Stage 7 — external communications

Connect the real CAN/RS485 equipment.

## Stage 8 — 32S isolation

Bring up the upper monitor as its own milestone.

## Stage 9 — pack validation

Only then does it make sense to start discussing production behavior.

---

# 🌍 Why this approach scales beyond STM32

This is where Zephyr earns its keep.

A future ESP32 target should not require this:

```text
copy project
rewrite main.c
rewrite protection
rewrite CAN
rewrite SOC
rewrite balancing
hope both versions stay synchronized
```

The intended port is:

```text
                COMMON BMS
                    │
       ┌────────────┴────────────┐
       │                         │
     STM32                     ESP32
       │                         │
 Devicetree/pins           Devicetree/pins
 I2C backend               I2C backend
 CAN backend               TWAI/CAN backend
 UART backend              UART backend
 flash backend             flash backend
 watchdog backend          watchdog backend
```

The following should stay essentially unchanged:

```text
bms_monitor
topology
snapshot
measurement
protection
state machine
balancing
estimator
configuration model
protocol codecs
system-health policy
```

If porting to ESP32 requires rewriting those modules, we have found an abstraction leak.

That makes the ESP32 port useful not only as a product feature, but as an architecture test.

---

# 🔀 Why changing the number of cells is a first-class problem

Cell count sounds like a configuration value.

Sometimes it is.

> [!IMPORTANT]
> Going from 12S to 16S may be mostly configuration. Going from 16S to 20S can change the *electrical architecture*: another AFE, another I2C path, another isolation boundary, different provider roles.  
> That is exactly why hardware agnosticism is not decoration in this project — it is a product requirement.

But crossing an AFE boundary changes the hardware architecture.

```text
8S  → one monitor, some channels unused
16S → one monitor, all channels available
20S → two monitor segments
32S → two full monitor segments
```

The BMS therefore separates:

```text
logical battery size
```

from:

```text
physical monitor topology
```

That allows the same high-level logic to ask:

```text
for every logical cell in the pack...
```

without knowing which chip owns it.

This matters for:

```text
protection
balancing
telemetry
diagnostics
min/max calculation
SOC support logic
future statistics
```

and is one of the main reasons the architecture is intentionally more structured than a typical single-board firmware.

---

# 🧬 CubeMX wasn't fired — it became the hardware reference

The legacy STM32 project was not thrown away.

It was used as a hardware reference.

CubeMX/HAL gave us valuable information such as:

```text
MCU                 STM32F446ZE
BQ I2C              I2C1 PB8/PB9
CAN1                PD0/PD1
RS485 UART          UART4 PC10/PC11
RS485 DE/RE         PB2
BQ ALERT            PC5
BQ RST_SHUT         PB14
BQ DFETOFF          PB6
ESP32 UART          UART5
GUI/debug UART      USART3
LCD I2C             I2C3
```

The difference is that this information now belongs in the **board description**, not scattered through the BMS logic.

CubeMX helped answer:

```text
"What is connected where?"
```

Zephyr now answers:

```text
"How can the application use that hardware without being married to it?"
```

---

# 🧯 Safety defaults

The STM32 profiles are intentionally conservative for initial bring-up.

Physical actuation should not be enabled simply because the code compiles.

The safe workflow is:

```text
build
  ↓
boot
  ↓
read BQ
  ↓
verify measurements
  ↓
verify GPIO polarity
  ↓
verify FET hardware
  ↓
enable actuation deliberately
```

The repository's development thresholds also exist to test the software.

They are not automatically suitable for a final LiFePO4 product.

---

# 🚧 Known deliberate limitations

## Automatic SOH learning

Not complete.

The project has:

```text
capacity-based SOH representation
retained full-capacity value
estimator plumbing
```

but not a production-qualified autonomous capacity-learning algorithm.

## Vendor inverter protocol

Not selected.

The CAN/RS485 reference protocols prove the communication architecture.

A specific inverter protocol should be implemented as an adapter once the target ecosystem is known.

## 32S upper GPIO routing

The 32S software profile contains a proposed/reference STM32 mapping for upper-AFE control signals.

That is a hardware design input, not evidence that a final PCB with those exact routes has been manufactured.

## Hardware validation

Still the largest missing milestone.

The architecture can tell us what should happen.

Only real hardware can tell us what actually happens.

---

# 📊 Project status — no marketing fog

If the question is:

> "Is the software architecture there?"

Yes, substantially.

If the question is:

> "Can the BMS behavior be exercised without hardware?"

Yes, through Zephyr `native_sim` and host tests.

If the question is:

> "Are the STM32 16S and 32S hardware profiles defined?"

Yes.

If the question is:

> "Has the final STM32 project been physically proven on a real 16S/32S power board?"

**No.**

That is the current boundary of the project.

```text
                 SOFTWARE SIDE
                 ─────────────
                 architecture       ✓
                 virtual BQ         ✓
                 topology           ✓
                 protection         ✓
                 balancing policy   ✓
                 estimator          ✓
                 config/persistence ✓
                 protocols          ✓
                 health supervisor  ✓
                 STM32 definitions  ✓ / needs target-build closure

                 HARDWARE SIDE
                 ─────────────
                 real I2C            pending
                 calibration         pending
                 ALERT               pending
                 FETs                pending
                 balancing current   pending
                 CAN transceiver     pending
                 RS485 transceiver   pending
                 32S isolation       pending
                 thermal             pending
                 EMC                 pending
                 pack validation     pending
```

That distinction is intentional and important.

---

# 📚 Documentation

Additional documents live under `docs/`.

Recommended reading order:

```text
README.md
   ↓
docs/DEEPSEEK_PROJECT_CONTEXT.md
   ↓
docs/BMS_Zephyr_v1.0_Technical_Manual.md
   ↓
docs/HARDWARE_16S.md
   ↓
docs/HARDWARE_32S.md
   ↓
docs/BUILD_MATRIX.md
```

The architecture notes in the repository go deeper into individual subsystems:

```text
CORE_RUNTIME_ARCHITECTURE.md
ESTIMATION_ARCHITECTURE.md
CONFIG_PERSISTENCE_ARCHITECTURE.md
COMMUNICATIONS_ARCHITECTURE.md
SYSTEM_SERVICES_ARCHITECTURE.md
```

---



# ⚡ The short version

If you only remember one diagram from this README, make it this one:

```text
                    Zephyr board definition
                            │
                            ▼
                     hardware backend
                            │
                            ▼
                       bms_monitor
                            │
                            ▼
                         topology
                            │
                            ▼
                       measurement
                            │
                            ▼
                         snapshot
                ┌───────────┼───────────┐
                ▼           ▼           ▼
           protection   balancing   estimator
                │
                ▼
          state machine
                │
                ▼
             outputs
```

And if the board changes:

```text
STM32 16S
   ↓
STM32 32S
   ↓
ESP32
   ↓
another AFE
```

the goal is that the changes stay near the top of that diagram:

```text
board definition
backend
monitor adapter
topology configuration
```

—not spread throughout the whole BMS.

That is the reason this project is built on Zephyr.

---

## 🧠 Final note

This repository is deliberately trying to make the **boring parts of a BMS boring**:

```text
where the pins are
which I2C bus is used
which AFE owns Cell 23
which backend sends CAN
which MCU happens to be fitted
```

Those details matter enormously, but they should not be allowed to infect every algorithm in the firmware.

Zephyr gives us the tools to keep them where they belong.

The end result should be a BMS where adding a second AFE, changing the cell count, or moving to a new MCU is a controlled platform change—not a reason to rewrite the product from scratch.

And that is really the whole project.
