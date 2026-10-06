# Core runtime architecture — v0.5

The runtime now has two independent decision outputs from the same coherent
snapshot:

```text
                         published snapshot
                              /       \\
                             /         \\
                    protection      balancing policy
                        ↓                 ↓
                 CHG/DSG allowed     desired global mask
                        ↓                 ↓
                  state machine         topology
                        ↓                 ↓
                switch/FET request   monitor-local masks
```

Important boundaries:

- measurement owns acquisition/publication
- protection decides what charge/discharge is allowed
- state machine owns CHG/DSG switch authority
- balancing policy decides which logical pack cells should balance
- topology performs physical routing to the correct monitor instances
- events are diagnostic history, not a safety source of truth


## Estimation consumer (v0.6)

Estimation is intentionally parallel to protection and balancing:

```text
published snapshot
      ├── protection
      ├── balancing
      └── estimator
```

Protection does not depend on SOC yet. That coupling should only be added when a
specific product requirement needs it.

The estimator owns coulomb/energy accounting, while measurement continues to own
the hardware observation and snapshot publication.


## Configuration boundary (v0.7)

Policy modules receive validated configuration. They do not own persistence.

```text
persistent store
     ↓
validated bms_app_config
     ↓
measurement / protection / balancing / estimator
```

Persistent storage is therefore replaceable independently from BMS policy.
