# Architecture and integration model

## Design goals

The C core is built around a simple rule:

> keep Polar protocol logic portable, and keep transport ownership in the application.

That is why most modules take an **ops struct** with function pointers instead of calling BTstack directly.

## Layers

### 1. Pure helper / protocol modules

These modules have no transport dependency and are the easiest to reuse:

- `polar_sdk_common.h`
- `polar_sdk_connect.h`
- `polar_sdk_wait.h`
- `polar_sdk_security.h`
- `polar_sdk_hr.h`
- `polar_sdk_pmd.h`
- `polar_sdk_ecg.h`
- `polar_sdk_imu.h`
- `polar_sdk_psftp.h`

They provide things like:

- retry timing,
- "wait until done or disconnected",
- HR packet parsing,
- PMD command building,
- ECG/ACC sample extraction,
- RFC60/RFC76 PSFTP framing.

### 2. Transport-facing helper modules

These still avoid owning the whole application, but they expect your code to provide transport callbacks:

- `polar_sdk_transport.h`
- `polar_sdk_gatt_mtu.h`
- `polar_sdk_gatt_control.h`
- `polar_sdk_gatt_write.h`
- `polar_sdk_pmd_control.h`
- `polar_sdk_psftp_runtime.h`
- discovery helpers under `polar_sdk_discovery*.h`

These modules encode application policy such as:

- how long to spend on each connect attempt,
- when to back off,
- how to enable CCC safely,
- when to request pairing,
- how to stage a PSFTP GET transaction.

### 3. BTstack adapters

These modules normalize BTstack events into simpler internal shapes:

- `polar_sdk_btstack_link.h`
- `polar_sdk_btstack_gatt.h`
- `polar_sdk_btstack_scan.h`
- `polar_sdk_btstack_sm.h`
- `polar_sdk_btstack_dispatch.h`
- `polar_sdk_btstack_helpers.h`
- `polar_sdk_btstack_adv_runtime.h`

They exist so the protocol/policy code does not need to know BTstack packet details everywhere.

## Typical application shape

In practice, a C application using the SDK looks like this:

1. Own a top-level app state struct
2. Register BTstack HCI / SM / GATT callbacks
3. Decode incoming events using the BTstack adapter helpers
4. Feed normalized state into runtime structs and discovery helpers
5. Use helper modules for control-plane actions
6. Parse streaming notifications into local buffers/rings

That is exactly the pattern used in:

- `examples/pico_sdk/main.c`
- `examples/pico_sdk_psftp/main.c`

## Why there is no hidden thread

The SDK does not create tasks, threads, or private schedulers.

Benefits:

- no surprise concurrency,
- deterministic ownership of timing,
- easy integration into existing embedded firmware,
- easier debugging when BLE timing goes wrong.

Cost:

- you must wire callbacks carefully,
- you must keep your state machine coherent,
- you must decide what your application does on disconnect / timeout / ATT auth failure.

## Runtime state

`polar_sdk_runtime.h` defines `polar_sdk_runtime_link_t`, a reusable link-status struct that tracks:

- current high-level state (`idle`, `scanning`, `connecting`, `discovering`, `ready`, `recovering`),
- whether the link is currently connected,
- connection handle,
- last HCI / disconnect status,
- connection update telemetry,
- disconnect counters by reason.

This is especially useful if you want the same status telemetry in both a C probe and another binding layer.

## Discovery model

The discovery helpers split the process into explicit stages rather than one monolithic function:

- find service,
- find characteristic,
- apply discovered handles,
- advance to the next stage,
- mark ready or mark failure.

That design matters because BLE discovery is asynchronous. The helper modules let you keep the stage policy in one place while your application continues to own the callback flow.

## Recommended integration strategy

If you are starting a new firmware integration:

1. Begin from `examples/pico_sdk/main.c`
2. Keep a single app state struct
3. Add `polar_sdk_runtime_link_t` immediately
4. Add discovery helpers next
5. Add HR first
6. Add PMD ECG/ACC second
7. Add PSFTP last

That order keeps the debugging surface small.