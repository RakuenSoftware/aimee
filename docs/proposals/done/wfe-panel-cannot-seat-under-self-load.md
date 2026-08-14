# Proposal: the WFE rt_gate panel cannot seat under the engine's own load

- **State:** DONE — saturation-aware routing archived 2026-08-04; capacity/deadline residual
  completed in Go 2026-08-14.

> **Completion notice.** Candidate occupancy influences group routing and route-selected admission
> races retry as backpressure. The completed
> [`wfe-panel-capacity-residual.md`](wfe-panel-capacity-residual.md) adds typed capacity,
> capacity-deadline, and execution-deadline states; scheduler retry; authoritative backend-health
> filtering coverage; and a repeated overlapping-roundtable campaign.

## Symptom

A live `build-e2e` run (`wi_f96d4b18…`, release `testing` @ `e161dd34`) cannot
pass `rt_gate`. Slice 1 paused three times with the same reason and the same
10-minute period:

```
21:27:46  pause  panel_unreachable: architect: delegate_error: context deadline exceeded;
                                    reviewer: delegate_error: context deadline exceeded
22:07:47  pause  (identical)
22:47:xx  pause  (identical)
```

Each attempt fails **exactly 10 minutes** after entering `rt_gate`
(`21:17:46` → `21:27:46`), then auto-resumes 30 minutes later and repeats. The
run burns its `autonomy.max_resumes` budget (50) making no progress.

## Mechanism

`roundtables/wfe.json` seats 3 personas plus a chairman, `turns: parallel`,
`min_successful: 2`, `deadline_ms: 600000`. Each seat picks `$random` from the
configured agents. Deployed `agents.json`:

| agent | model | max_parallel | timeout_ms |
|---|---|---|---|
| codex | gpt-5.6-sol | 10 | 600000 |
| MiniMax-M3 | MiniMax-M3 | 4 | 180000 |
| claude | (unset) | **unset → 3** (`AGENT_DEFAULT_MAX_PARALLEL`, `agent_types.h:47`) | 600000 |
| local-gemma4 | aimee-synth | 2 | 300000 |
| kimi-k2.7-code | kimi-k2.7-code | unset → 3 | 180000 |

Two seat-level failure modes were observed directly in the job queue:

```
job 14813  review  failed     claude       agent 'claude' at concurrency limit (max_parallel=3)
job 14821  review  failed     claude       agent 'claude' at concurrency limit (max_parallel=3)
job 14812  review  cancelled  MiniMax-M3   cancelled: WFE turn cancelled
                              (created 22:37:47, cancelled 22:47:51 — the 600000ms panel deadline)
```

- A `claude` seat fails **immediately** at 3 concurrent.
- A `MiniMax-M3` seat is **cancelled mid-work** (turn 7–8) at the panel deadline.

With two slices at `rt_gate` at once, the engine asks for up to 8 concurrent
agents. `claude` saturates at 3 and its seats fail instantly; slow seats are
killed at 600s. Fewer than `min_successful: 2` seats return, so the panel is
declared unreachable. **The engine starves itself** — no external load is
required.

**This is load-dependent, not deterministic.** Slice 1 passed `rt_gate` on its
fourth attempt at 23:21:05, and slice 2 passed on its first at 23:10:19. The
defect makes seating unreliable under the engine's own concurrency; it does not
make it impossible. Any claim that the gate "cannot" be passed is too strong —
what is established is that it fails a substantial fraction of the time, with a
failure mode that costs a full 10-minute deadline and a 30-minute backoff each
time.

## Why the agent pool is the suspect, not the thread pool

The natural suspicion is the server compute pool, but that is a dead end and is
recorded here so it is not re-investigated: `9fd1b2c0` ("match roundtable worker
capacity") raised `CONFIG_DEFAULT_BACKGROUND_THREADS` 2 → 14 "matching the
standard 10 Codex + 4 MiniMax roundtable capacity", and `840132c8` ("decouple
roundtable admission from compute workers") then **deliberately reverted it to
2** because admission no longer runs through compute workers. Both are ancestors
of the deployed release — verified, not assumed:

```
$ git merge-base --is-ancestor 9fd1b2c0 origin/testing && echo YES    # YES
$ git merge-base --is-ancestor 840132c8 origin/testing && echo YES    # YES
$ git show e161dd34:src/modules/config/config.h | grep BACKGROUND_THREADS
#define CONFIG_DEFAULT_BACKGROUND_THREADS 2
```

The deployed release therefore carries the revert, and the revert is correct.

This exclusion is **evidence-based but not dispositive**: it shows the specific
thread-count hypothesis is wrong, not that the deployed runtime is unconstrained
elsewhere (a per-queue admission limit, say, was not traced). What rules the
thread pool out as *the* constraint at the observed failure time is the direct
evidence: the failing seats name a per-agent concurrency limit, not worker or
queue starvation.

That commit pair does, however, record the intended roundtable capacity:
**10 Codex + 4 MiniMax = 14**. `claude` and `local-gemma4` are not part of it,
yet `$random` can seat them.

Note also that `local-gemma4` targets `aimee-synth`, i.e. the `aimee-llm`
gateway. Whenever that container is down — as it is now, deliberately, for
replacement-LLM testing — every seat that draws `local-gemma4` fails. Taking
`aimee-llm` down therefore silently degrades the roundtable panel. That coupling
is not obvious from either side.

## Established vs. assumed

Established: the pause timestamps and 10-minute period; the three job records
quoted above; the `agents.json` values; `AGENT_DEFAULT_MAX_PARALLEL == 3`; the
`wfe.json` seat/deadline/`min_successful` settings; the commit pair above.

### Root cause: routing is not saturation-aware

`$random` resolves through `agent_route_*` in `src/modules/routing/routing.c`,
which filters candidates on `enabled`, role support, cost tier, and
`agent_is_available_for_routing()`. That last call delegates to
`agent_routing_block_reason()`, which checks:

- provider health (`g_route_health_filter` → `AGENT_ROUTE_HEALTH_DOWN`),
- the client-only-claude structural rule,
- the policy filter (`primary_only` etc.),
- backend command availability (tmux/CLI on PATH).

**It does not check whether the agent has a free concurrency slot.** A saturated
agent remains a fully valid routing candidate, is selected, and only then fails
at admission (`agent_runtime.c:259`) with `AGENT_RC_AT_LIMIT`.

The health path cannot compensate, and deliberately so: `agent_fallback.c:61`
records that the "at concurrency limit" message is classified **non-retryable so
at-limit never records health**. That is correct in itself — saturation is not a
provider fault and must not poison health — but it means a saturated agent is
never marked `HEALTH_DOWN` either. It therefore stays selectable indefinitely.

So routing and admission disagree: routing believes the agent is available;
admission knows it is full.

One step in that chain is **not verified**, and the claim is limited accordingly.
`agent_runtime.c:255` sets `AGENT_ADMIT_NONBLOCKING` only when
`tl_admission_fail_fast` is set; otherwise admission *blocks* and waits for a
slot. Which caller sets that flag for a panel seat was not traced. What is
observed is that panel seats **did** take the fail-fast path — jobs 14813 and
14821 failed instantly rather than waiting — so the flag is evidently set
somewhere on this path, but the responsible caller has not been identified.

Limited to what that supports: on the observed fail-fast path, a seat drawing a
saturated agent fails immediately, and with `min_successful: 2` a couple of such
draws take down the gate. Whether *every* seat takes that path is unestablished.

Assumed, NOT established:

- **That `MiniMax-M3` would finish given more time.** It was cancelled while
  still advancing turns; whether it converges at, say, 20 minutes is untested.
- **Whether `claude`'s cap of 3 is deliberate.** It is unset in `agents.json` and
  falls through to the compile-time default. Whether that reflects a provider
  rate limit or simply nobody setting it is unknown, and it determines whether
  option 1 is safe.

## Options

1. **Give `claude` an explicit `max_parallel`** matching its real provider
   limit. Smallest change. Unsafe until the third assumption above is settled —
   raising it past a real rate limit trades one failure for another.
2. **Restrict `$random` to the intended capacity pool** (`codex`, `MiniMax-M3`),
   excluding `claude` and `local-gemma4`. Matches the documented design intent.
   Cost: a smaller pool, and it hard-codes a roster that was meant to be dynamic.
3. **Make an at-limit seat wait rather than fail.** `agent_runtime.c:255` already
   blocks by default (`flags = tl_admission_fail_fast ? AGENT_ADMIT_NONBLOCKING : 0`)
   and `AGENT_RC_AT_LIMIT` is meant to drive fallback to another agent. Panel
   seats are evidently taking the fail-fast path. Making them queue or fall back
   is the most principled fix. Cost: needs the fail-fast caller identified first;
   a queueing seat also consumes panel deadline while waiting.
4. **Exclude agents whose backend is unreachable** from seat selection, so a
   downed `aimee-llm` cannot silently cost the panel a seat. Complementary to any
   of the above, not a substitute.
5. **Raise `deadline_ms`** so slower seats finish. Addresses only the cancellation
   half, not the `claude` saturation half, and lengthens every failure.

6. **Make routing saturation-aware.** Add a "no free slot" block reason to
   `agent_routing_block_reason()` so a saturated agent is not a candidate, while
   leaving health classification untouched (saturation still must not be
   recorded as a provider fault). Routing then picks a peer that can actually
   run, which is what the `$random` pool exists to provide.

## Recommendation

**Option 6**, with option 3 as its complement.

The defect is that routing and admission disagree about what "available" means.
Options 1, 2 and 5 all tune numbers so the disagreement stops being reachable in
one particular configuration; none of them removes it. Raise `claude`'s cap and
the same failure returns at a higher slice count; restrict the pool and it
returns whenever the narrowed pool saturates.

Option 6 removes the disagreement at its source and needs no knowledge of any
provider's real rate limit, which is the fact that makes option 1 unsafe today.
It also preserves the `agent_fallback.c` invariant: the fix is in *selection*,
not in health classification, so saturation still never counts as a fault.

Option 3 remains worth doing for the residual race — an agent can saturate
between selection and admission — so a seat that loses that race should fall
back or wait rather than fail. Option 6 makes that race rare; option 3 makes it
harmless.

Option 4 should be taken regardless. It is a correctness bug independent of
load: with `aimee-llm` deliberately stopped, any seat drawn against
`local-gemma4` is a guaranteed failure. (This may already be covered by the
existing health filter — `AGENT_ROUTE_HEALTH_DOWN` — if the catalog marks the
dead gateway down; that was not verified, and should be before implementing
anything separate.)

## Acceptance

The defect is load-dependent and intermittent, so a single green live run proves
nothing. Acceptance is therefore stated as deterministic tests, with the live run
as supporting evidence only.

Deterministic:

- **Saturated pool:** with agent A at its limit and agent B free, seat selection
  returns B, not A. No seat fails with `concurrency_limit`.
- **Empty eligible pool:** with *every* eligible agent saturated, selection
  returns a distinct, named "no agent with free capacity" condition — not a bare
  "no agent" error, and not a seat that fails at admission.
- **Concurrent-admission race:** when N seats select simultaneously against a
  pool with fewer than N free slots, each seat either obtains a slot or reports
  the no-capacity condition; none fails with a generic provider error.
- **Deadline expiry:** a seat still waiting when the panel deadline elapses is
  reported as deadline-expired-while-waiting, distinguishable from a provider
  fault and from no-capacity.
- **Health-classification invariant preserved:** a seat that hits the
  concurrency limit still does **not** record provider health (the
  `agent_fallback.c:61` invariant), verified by a test.
- **Unreachable backend:** with `aimee-llm` stopped, no seat is assigned to
  `local-gemma4`.

Supporting evidence, not a gate:

- Across at least 10 `build-e2e` runs with two or more slices at `rt_gate`
  simultaneously, no run pauses with `panel_unreachable`. A single passing run is
  explicitly insufficient, since slice 2 passed on its first attempt even with
  the defect present.
- A pause caused by unfillable seats says so, rather than reporting a generic
  deadline-exceeded.
