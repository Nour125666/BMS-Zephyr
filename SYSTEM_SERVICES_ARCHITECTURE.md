# System services architecture — v0.9 final virtual milestone

This is the final generic/virtual platform milestone before target-hardware
integration.

```text
                         SYSTEM SUPERVISOR
                      /         |          \
             startup/self-test health      watchdog
                    |          |              |
                    |          +--- safe latch|
                    |                         |
                    v                         v
measurement -> snapshot -> core runtime -> outputs
                         |
                         +-- communications
```

## Startup / self-test

The supervisor checks:

- validated applied configuration
- topology presence / logical cell count
- one fresh measurement acquisition
- one core protection/estimator processing cycle
- communications telemetry collection
- persistent-store readability (empty first-boot store is allowed)

Failure latches the system safe.

## Health supervisor

The periodic health loop watches:

- missing/stale measurement snapshots
- measurement errors/stopped/no-progress
- core runtime errors/stopped/no-progress
- communication errors/stopped/no-progress

Battery-control failures are **critical** and latch SAFE.

Communication failures are **degraded**: battery protection keeps operating, but
the system reports `DEGRADED`.

## SAFE_LATCHED

SAFE is deliberately latched.

On entry:

```text
core runtime periodic loop stops
balancing mask -> 0
CHG/DSG control -> ALL_OFF
```

The BMS does not automatically return to normal because a transient health problem
disappeared.

Recovery requires:

```text
bms system recover
```

and succeeds only if a fresh measurement and core decision can be rebuilt.

## Maintenance mode

Configuration apply intentionally stops/reconfigures/restarts measurement/runtime.
The supervisor enters `MAINTENANCE` during that controlled operation so it does not
mistake maintenance for a fault.

## Watchdog policy

The supervisor owns the watchdog-feed decision.

It feeds only while:

- startup/self-test has passed
- the system is not SAFE_LATCHED
- the supervisor itself is running

A degraded communication link does not stop feeding because battery-control logic
can still be healthy.

`bms_watchdog_backend_ops` is hardware-neutral.

v0.9 uses a virtual watchdog backend. On STM32 this backend becomes Zephyr's
hardware watchdog driver.

## Regression scenarios

`bms scenario all` runs deterministic virtual scenarios for:

- normal pack
- software UV
- software OV
- discharge overcurrent
- charge overcurrent
- BQ hardware UV fault path
- balancing selection
- CAN + RS485 telemetry

This complements layer smoke tests with behavior-level regression tests.
