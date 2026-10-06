# Configuration + persistence architecture — v0.7

## Why configuration is its own domain object

Before v0.7, development thresholds lived as `static const` values in `main.c`.

They are now grouped into:

```text
bms_app_config
├── measurement/runtime periods
├── protection limits
├── balancing policy
└── estimator setup
```

This means the BMS logic no longer needs to be recompiled just because a validated
product configuration changes.

The topology is intentionally **not** in this persisted object yet. Topology is a
board/product hardware description and will ultimately come from board
configuration / Devicetree rather than a casual field setting.

## Staged vs applied

Configuration edits are staged first:

```text
STAGED config
      ↓ validate
bms config apply
      ↓
APPLIED config
```

This prevents a half-edited threshold set from immediately changing safety policy.

## Persistent image

The store does not dump a C struct directly into flash.

It serializes fields explicitly in a versioned binary format:

```text
header
├── magic
├── store format version
├── payload length
├── generation
└── CRC-32

payload
├── bms_app_config
└── retained estimator state
```

The CRC detects corruption. Schema/version checks reject incompatible images.

## Estimator state

The retained block saves useful runtime values:

```text
remaining charge
estimated usable full capacity
charged/discharged/throughput counters
energy in/out
skipped interval counter
```

Timing anchors and fractional integration remainders are intentionally not
persisted. After restore, the estimator re-anchors on the next fresh snapshot.

## Native simulator backend

`bms_config_store_virtual.c` emulates non-volatile storage inside one native_sim
process.

It supports:

```text
save
load
erase
simulate_reboot
```

It is enough to validate serialization, CRC, versioning, staged/applied behavior,
and runtime restoration.

It deliberately does **not** claim persistence after the native_sim executable
itself exits.

## Real STM32 backend later

The same generic store interface will later be backed by Zephyr Settings/NVS or a
dedicated flash partition:

```text
bms_config_store
       ↓
Zephyr Settings/NVS
       ↓
STM32 internal/external flash
```

The upper BMS modules do not change.
