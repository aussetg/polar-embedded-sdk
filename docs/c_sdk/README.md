# C SDK

The C SDK is the **portable Polar-specific core** under `polar_sdk/core/`.

It is designed for a competent embedded developer who is comfortable with:

- event-driven BLE flows,
- transport adapters and callback tables,
- explicit state machines,
- owning the final application logic.

## What the C SDK is

The C SDK is a collection of **small reusable modules** that solve the hard parts of talking to Polar devices:

- connect retry and backoff policy,
- normalized link/runtime state,
- discovery progression helpers,
- GATT write / CCC / MTU helpers,
- security retry policy,
- Heart Rate parsing,
- PMD control-path encoding and response handling,
- ECG and ACC notification parsing into byte rings,
- PSFTP framing, request/query encoding, response reassembly, and directory decoding.

## What the C SDK is not

It is **not yet** a complete, transport-independent, one-call session library.

The public facade in [`polar_sdk/core/include/polar_sdk_session.h`](../../polar_sdk/core/include/polar_sdk_session.h) defines the shape of that future API, but most operational calls currently return `POLAR_ERR_UNSUPPORTED`.

Today, the real production-ready surface is the set of helper headers in:

- `polar_sdk/core/include/`

The best examples of how to compose them are:

- `examples/pico_sdk/main.c`
- `examples/pico_sdk_psftp/main.c`

## Documentation map

- [Architecture and integration model](./architecture.md)
- [Public API and status](./public_api.md)
- [Core helper modules](./helper_modules.md)
- [C integration cookbook](./cookbook.md)

## Mental model

Think of the C SDK as three layers:

1. **Protocol facts encoded as helpers**
   - parsers, frame builders, response decoders
2. **Policy helpers**
   - retry logic, timeout loops, security escalation, MTU gating
3. **Integration glue points**
   - callback tables that let your BTstack-facing application plug the helpers into a real run loop

That split is deliberate:

- protocol logic stays testable and portable,
- transport logic stays explicit,
- MicroPython and pure-C probes can share the same core code.