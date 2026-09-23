# Bounded evidence recovery proposals — 2026-09-23

The current-state requirement evaluator now produces an optional recovery plan
for missing required subject/relation roles. The plan is metadata, not prompt
content or permission to execute a tool. It binds the task revision, requirement
digest and exact retained selection digest. The host must admit any work under
its own authenticated task and operator budgets.

`evidence_requirements.recovery_budget` declares maximum rounds, new items,
estimated tokens, elapsed milliseconds and cost microunits. Version 1 admits at
most one round, 16 new items, 4096 estimated tokens, 2000 milliseconds and zero
external model cost. Zero work in any required dimension disables proposals.
These are proposed ceilings; this change does not execute work or claim measured
consumption. The shared token/item ceilings apply across the whole proposed round.

Only missing required current-state roles propose `lookup_current_role`.
Budget-dropped evidence requests a host packing revision; conflicts require
review; unavailable sources remain unavailable. Optional roles do not initiate
work. A stable attempt key binds planner version, task revision and exact role,
independent of query wording or selection changes. It enables subsequent host
attempt deduplication; durable attempt tracking is not implemented here.

The owner regenerates the plan after outer packing and does not import serialized
actions as authority. Coverage remains insufficient until retained evidence
actually satisfies the obligation. Unsupported query modes remain unknown.
Requirements now reject duplicate keys, case aliases, null fields, invalid UTF-8
and trailing data at both public and direct owner decoding boundaries, including
nested obligations and recovery budgets. Failed decoding preserves the previously
admitted value.

The targeted Go coverage, recovery, typed-context and ingress race suite passes;
the independently exported owner builds and executes the new regressions. The
rebuilt native Go-owner ingress fixture passes, as do module inventory, descriptor
and memory ownership checks. [Race](memory-recovery-plans-2026-09-23/go-race.txt),
[native](memory-recovery-plans-2026-09-23/native.txt) and
[export](memory-recovery-plans-2026-09-23/export.txt) logs are retained.
Fresh application/harness `0a63ad568` passes **1,110/1,110 checks**: 110 T1
and 1000 T2. Both topologies exercise the new proposal-only and packed-away-role
HTTP cases. The enrolled Server additionally passes provider-boundary, native
async refusal/recovery, restart, outage and isolation checks. The
[summary](memory-recovery-plans-2026-09-23/fresh/summary.json),
[T2 image identities](memory-recovery-plans-2026-09-23/fresh/image-identities.json)
and [environment](memory-recovery-plans-2026-09-23/fresh/environment.json) are
retained with raw receipts. Six T2 image identities and both actual caps were
captured; T1 completed before independent image capture. T3 was not rerun for
this opt-in KB planner change. Released 0.4.5 on CT100 remained healthy.

Host admission/execution, durable duplicate-attempt recording, authenticated
outcome application and timeline/source-chain expansion remain open. This is an
MR-05 planner slice, not completion of MR-05 or the recovery-loop acceptance gate.
