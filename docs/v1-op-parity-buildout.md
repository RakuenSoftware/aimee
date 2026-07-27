# `/v1` route parity

The named `/v1` migration is complete. The generic RPC passthrough is retired.

## Contract

Every public operation has:

- one named route;
- one stable operation ID;
- declared authentication, capabilities, scope, write tier, body bounds, and execution class;
- a handler in the owning service;
- OpenAPI and generated reference coverage;
- a thin-client route when the CLI exposes it.

No-argument reads use `GET` where the response is a resource view. Parameterized reads and mutations
use typed JSON bodies. Long work returns a run/job handle or uses a declared stream; it does not block
the listener indefinitely.

Internal hooks, runner channels, primary-session operations, tool execution, and workflow resource
routes have dedicated privileged paths. They are not public merely because they use `/v1`.

## Gates

```bash
make -C src api-conformance-check
make -C src server-api-conformance-check
make -C src v1-method-coverage-check
make -C src cli-v1-routes-check
make -C src v1-route-order-check
make -C src docs-gen-check
```

A new operation lands only when descriptor, handler, OpenAPI, client exposure, auth tests, and
generated docs agree.
