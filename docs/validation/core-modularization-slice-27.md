# Core modularization slice 27: roundtable activation enforcement

## Scope

This slice makes the optional `roundtable` descriptor operational at its execution boundaries. It adds the canonical
`modules.roundtable` tri-state, an owner-local environment fallback, central execution guards, explicit
CLI/server diagnostics, and terminal workflow behavior for an intentionally disabled module.

It does not remove linked objects or de-register routes. Those remain separate build-profile and
capability-registration work so this slice does not overstate physical optionality.

## Activation contract

Resolution is `config_module_enabled(cfg->module_roundtable, env_default)`: explicit config `0` or `1`
wins; only an absent `-1` value consults `AIMEE_MODULE_ROUNDTABLE`. The environment accepts
case-insensitive `1|true|on|yes` and `0|false|off|no`. Missing, empty, whitespace-padded, and invalid
values resolve disabled, matching `module.yaml`'s `enabled_by_default: false`. There is no administrative
hot-toggle surface; callers resolve the setting through the owner API at execution boundaries.

## Execution evidence

The only non-test engine callers at implementation time are:

- `src/cmd_agent_delegate.c`: `delegate_ensemble_run`
- `src/server/server_compute.c`: `delegate_ensemble_run`
- `src/server/server_compute.c`: `delegate_roundtable_run`
- `src/modules/workflows/wfe_live_panel.c`: `delegate_roundtable_run`

Both engines reject disabled execution before fan-out, providers, chairs, verification, or model calls.
The CLI and server check before loading agent configuration. HTTP aggregate/roundtable routes, MCP
`ensemble_review`, and the authoring pipeline submit the same server methods and inherit their explicit
terminal error. The workflow provider checks before agent loading, preset/seat resolution, task
construction, or seat waiting; `WFE_PANEL_MODULE_DISABLED` maps to a permanent workflow failure rather
than the transient `panel_unreachable` retry path.

Core delegate execution, voting, tracing, and generic `agent_run_parallel` consumers are not gated.
`dev.sweep` has no reference to either roundtable engine and is not part of this activation boundary.

## Deferred work

- omit roundtable implementation objects from disabled generated Make/CMake profiles;
- conditionally register and advertise CLI, HTTP, MCP, pipeline, preset, and frontend surfaces;
- project only active roundtable configuration into the applicable web GUI.

## Verification

- config surface and save/load tests cover true, false, and absent tri-state persistence;
- activation tests cover every accepted environment spelling, invalid fail-closed behavior, and
  config-over-environment precedence;
- engine tests assert zero parallel, named-agent, or aggregator calls while disabled;
- workflow tests assert module-disabled is terminal while real provider-unreachable behavior remains
  transient;
- module documentation and the parent proposal state the linked-surface limitation explicitly.
