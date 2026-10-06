# Estimation architecture — v0.6

The estimator is deliberately another **consumer of the published snapshot**:

```text
BQ hardware / virtual BQ
        ↓
measurement
        ↓
published snapshot
        ├──────── protection
        ├──────── balancing
        └──────── estimator
                     ↓
              SOC / charge / energy
```

It never reads the BQ directly.

## SOC

SOC is currently calculated by coulomb counting:

```text
remaining charge += current × time
```

Project current convention:

```text
+ current = charging  → SOC rises
- current = discharge → SOC falls
```

The estimator integrates only **new snapshot sequence numbers**. The core runtime
runs faster than measurement, so duplicate snapshots are ignored.

An unexpectedly long measurement gap is not integrated. The estimator re-anchors
instead of pretending it knows what happened while data was missing.

## SOH

v0.6 does **not** invent an automatic SOH-learning algorithm.

The reported capacity-based SOH is:

```text
estimated usable full capacity / design capacity
```

At startup both are 100 Ah in the simulator, therefore SOH = 100%.

The API already allows a future persisted/capacity-learned value to update the
estimated full capacity. Example:

```text
bms estimator set_full_capacity 80000
```

with a 100 Ah design capacity gives a capacity-model SOH of 80%.

A real automatic SOH learner needs validated cycle/anchor logic and is intentionally
left for a later estimator revision rather than faking a number.

## Counters

The estimator also tracks:

- charged uAh
- discharged uAh
- total throughput uAh
- energy in uWh
- energy out uWh
- discharge-based equivalent full cycles

These counters will later be natural candidates for persistent storage.
