# config module

## Purpose and non-goals

`config` is required core and owns configuration defaults, parsing, validation, persistence, effective
snapshot publication, reload classification, and projections consumed by CLI, API, environment, and GUI
surfaces. It does not own module behavior, provider selection semantics, secret values, deployment
orchestration, or a GUI's navigation and presentation.

## Public contracts

`config_load`, `config_load_file`, `config_save`, `config_snapshot_get`, `config_reload`, and
`config_reload_if_changed` are the current C contracts in `src/modules/config/config.h`. The typed
`config_fields` allowlist supplies get/set validation, surface grouping, and the `live` or
`restart_required` verdict. Module/provider owners remain responsible for consuming their values and
registering a re-applier when bound state can safely change live.

## Dependencies and consumers

- `module-runtime`: supplies the required module identity, lifecycle, and readiness model whose active capabilities constrain configuration surfaces.

Every required and optional module may consume `config`; server startup, the live server loop, CLI
commands, deployment tooling, runtime and control GUIs, and tests are direct consumers. A dependency on
config does not transfer ownership of another module's schema or activation semantics into this module.

## Providers and readiness

The reference provider is `aimee.yaml` plus compiled defaults and typed projections. Environment
overrides are narrow consumer-owned seams, such as `config_apply_db2_url_env_override`, rather than a
second universal parser. Readiness distinguishes missing files, accepted defaults, rejected invalid
reloads, persistence failures, and startup-bound values; a syntactically valid snapshot does not prove
that its selected provider or module is operational.

## Configuration and activation

- `runtime_toggle.supported`: `false`; configuration is required core and cannot be disabled, while the values it projects may activate optional modules and providers.

Defaults are established before file parsing. Server startup seeds a lock-free snapshot; `config.set`
performs a disk-based read-modify-save and immediately calls `config_reload`; the main loop also detects
out-of-band file changes once per tick, while SIGHUP requests reload off the signal path. Invalid reloads
retain the running snapshot. `RELOAD_RESTART` fields persist immediately but do not claim live effect.

### GUI truthfulness contract

A field shown in the GUI exists only when the currently selected live module/provider consumes it; unused fields are not rendered, not defaulted, and not silently preserved across reload of a module/provider that does not consume them.

The current main Settings page receives every allowlisted field and filters only by runtime/deploy/
advanced/dev group; the webchat settings panel uses a static allowlist. Dynamic live-consumer filtering
is `not present`, so the quoted rule is the required target boundary rather than a current guarantee.

## Surfaces

Surfaces include `aimee config show|get|set`, `/v1/config`, `/v1/config/get`, `/v1/config/set`, typed
`config_t` readers, `aimee.yaml`, narrowly defined environment overrides, and GUI projections. All
surfaces must derive from one effective contract and expose an accurate reload verdict; a GUI may curate
or label fields but may not invent defaults, persistence, provider readiness, or activation state.

## Data and migrations

Configuration data comprises compiled defaults, the parsed YAML tree, flat typed `config_t` values, the
file-identity cache, the published double-buffer snapshot, field metadata, and consumer-specific derived
state. Save/load round trips are compatibility-sensitive migrations: recognized sections must not be
silently dropped, unknown or legacy shapes require an explicit policy, and secrets should be stored as
`vault` references rather than copied into general configuration.

## Security and privacy

Configuration files, environment values, API writes, paths, endpoints, commands, and provider metadata
are untrusted input. The typed `config_fields` allowlist bounds remote get/set, strict validation rejects invalid
reloads, and disk reads avoid overwriting unseen edits. Config must not expose secret material through
GUI/API projections, logs, reload diagnostics, serialized defaults, or an inactive module's stale field.

## Supported journeys

At startup, defaults and a valid file produce the initial effective snapshot before consumers bind
state. During operation, `config_reload` validates and atomically publishes a typed API write or detected
file change; hot readers see the next snapshot, registered re-appliers update bounded state, and
startup-bound consumers continue with an explicit restart requirement. CLI-only reads use the file path
when no live server snapshot exists.

## Tests and failure behavior

`test_config.c`, `test_cmd_config.c`, `test_config_surface.c`, `test_config_snapshot.c`, config parser
suites, and frontend setup/settings tests cover defaults, validation, round trips, projections, reload,
and UI assumptions. Missing files yield defaults; malformed or invalid reload input keeps the running
snapshot; failed save/set returns an error; unrecognized typed keys are rejected rather than persisted.

## Operational diagnostics

Report the selected config path, validation issue class, reload source, `publication/no-op/rejection`,
field reload verdict, re-applier failure context, and module/provider readiness without printing values
that may be secret. SIGHUP and file-watch logs must distinguish a rejected reload from a process restart;
neither path restarts the server in the current implementation.

## Compatibility

Field names and `config_t` types, defaults, saved YAML shapes, allowlisted API response shapes, environment
precedence, reload verdicts, and preservation on save are compatibility contracts. Adding a field is not
enough to make it user-facing: its consuming module/provider, activation condition, secrecy, reload
class, and live GUI eligibility must also be declared and tested.

## Extension and removal

New configuration belongs with the module/provider that consumes it and is projected through config's
shared validation and persistence seams. Duplicate parsers, hand-maintained GUI field lists, settings
with no non-test consumer, and serialize-only values are `configuration-only` or
`duplicated-by-adjacent-module` candidates, not confirmed dead code; removal requires save/load,
environment, API, GUI, and runtime-liveness evidence.
