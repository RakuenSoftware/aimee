# config module

## Purpose

`config` is a required pure-Go process. It owns configuration defaults,
YAML parsing, validation, optimistic versioning, and atomic persistence.
Callers do not receive a store or filesystem path and cannot import the
implementation; they use the caller contract in `server-go/config`.

The module contains no C implementation, compatibility layer, or bridge.

## Event-bus contract

The module serves principal `1/2`, event kind `4609`, stage `1`
(`config-store`). Requests and responses are JSON. The owned operation catalog
and golden vectors are in
`src/modules/config/eventcontract/operations.json`.

Supported operations are:

- `values`: return the editable configuration projection with defaults.
- `value`: return one editable value.
- `string-value`: read a non-editable string needed by an internal consumer.
- `set-versioned`: validate and atomically persist a value, optionally guarded
  by the previous version.
- `version`: return the SHA-256 version of a configuration node.
- `trigger-rules`: return validated workflow trigger rules.
- `snapshot`: return one consistent effective-value/version pair.
- `persist-defaults`: persist missing public defaults without writing secrets.
- `workspace-add` / `workspace-remove`: mutate the bounded workspace registry.
- `apply-roundtable-preset`: atomically apply one complete preset.
- `set-typed-facts`, `set-api-http-listener`, `set-model-concurrency`, and
  `remove-model-concurrency`: perform multi-field mutations under one store lock.

Every call has a bounded deadline. Malformed JSON, unknown fields, trailing
values, wrong stages, and oversized replies fail closed. Validation errors stay
inside a successful bus reply as typed `code`/`error` data because a non-OK
bus status intentionally carries no body.

## Storage and startup

Only the module resolves its file. `AIMEE_CONFIG_PATH` selects an explicit
path; otherwise it uses `$AIMEE_HOME/aimee.yaml`, falling back to the user's
configuration directory when `AIMEE_HOME` is unset.

Writes hold the store mutex, preserve unrelated YAML nodes, write a mode-0600
temporary file in the destination directory, sync and close it, then rename it
over the configured path. Version checks happen under the same lock.

## Consumers

The Go workflow engine creates one concurrent event-bus caller and shares it
with DB1, config, delegates, and other module clients. The WFE waits for both
DB1 and config to answer before serving. Its HTTP config endpoints and scheduler
depend only on the `config.Service` caller interface.

## Tests and failure behavior

Both sides replay the same owned golden vectors:

- `server-go/config/contract_test.go` proves the caller emits the contract
  bytes, event kind, and stage, and decodes typed responses.
- The external module's `server-go/modules/config/contract_test.go` proves the
  module accepts those bytes and emits the matching responses.
- `src/tests/test_bus_config_autonomy.c` proves native callers emit the same
  event/stage/JSON envelopes through the generic module transport.
- External handler/store tests cover malformed payloads, validation, atomic
  persistence, trigger rules, versions, conflicts, and concurrent mutation.
- Deployment E2E starts the independently built config process and WFE against
  the daemon bus, then exercises reads, writes, persistence, conflicts, restart,
  and invalid-input behavior through the public HTTP surface.

Missing files produce defaults. Invalid values do not modify the file. A stale
version returns a conflict. An unavailable config process prevents WFE startup
instead of silently falling back to direct file access.
