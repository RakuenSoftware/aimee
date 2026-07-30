# WFE panel capacity path

The shipping panel seat path starts in `server-go/internal/engine/agent_client.go`.
`planDelegateGroup` reads `/v1/agent/list`, applies role, persona, enabled,
primary-only, command-availability, policy, provider-health, and admission-capacity
eligibility, and reserves the reported global, per-agent, and shared-model
headroom across the group before any seat is dispatched. An eligible but
initially saturated pool returns `ErrNoFreeDelegateCapacity` without pinning an
agent or creating a job.

The list is produced by `src/server/server_agent.c`. It exposes the locked,
non-mutating probe in `src/server/agent_admission.c`; that probe and atomic
acquisition share `capacity_locked`, so routing and admission use the same three
limits. `src/server/agent_route.c` installs this probe as a hard routing filter.
These `src/server` files are the compiled server path; similarly named
`src/modules` files are not authoritative for WFE admission.

Selection is still only a snapshot. `src/server/agent_runtime.c` performs the
atomic acquisition immediately before execution. Fail-fast calls report
`AGENT_RC_AT_LIMIT`; blocking calls wait on the existing admission condition
variable and poll cancellation, bounded by the caller's panel context deadline.
Every acquired `agent_slot_t` is released by runtime cleanup on success, error,
timeout, or cancellation. `src/server/agent_fallback.c` explicitly excludes
`AGENT_RC_AT_LIMIT` from provider-health accounting, so saturation never marks a
provider unhealthy.

Go cancellation in `delegateOnce` cancels and forgets the remote job before
returning. Seat errors retain typed capacity and context deadline/cancellation
identity through `DelegateGroup`; `runPanelAnalysis` translates those to
`no_free_capacity` and `deadline_expired_while_waiting`. The roundtable pause is
therefore respectively `panel_no_free_capacity` or `panel_admission_deadline`,
while ordinary eligibility and provider failures remain `panel_unreachable`.

Backend reachability is not a second capacity signal. The production
`aimee-synth` / `aimee-llm` failure and recovery path updates the existing route
health state used by `src/server/agent_route.c`; while down, `local-gemma4` is
excluded by the provider-health filter, and successful recovery restores normal
eligibility.


Capacity evidence belongs in deterministic admission, routing, planner, race,
and deadline tests. Repeated `build-e2e` runs are supporting operational evidence
only; they are not a substitute for those release-gate tests.
