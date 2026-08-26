# Turn integrity core

The turn-integrity core provides protocol-neutral, caller-owned contracts for
the lifecycle of an Aimee turn. It owns bounded schemas, state transitions, and
an optional observation hook. It does not own authorization, persistence,
provider transport, or audit storage.

The server installs a bridge from the observation hook to the durable event bus.
Other binaries can use the same state machine without linking server or audit
dependencies.

## Invariants

- A turn identity exists before work is dispatched.
- State transitions are monotonic and terminal states cannot be reopened.
- Configuration, toolset, routing, policy, and context snapshots bind once.
- Events carry bounded metadata only; prompt, tool argument, result, and model
  response content are excluded.
- Mechanical policy remains authoritative. Observability is not an
  authorization boundary.

## Retrieval outcomes and continuations

Retrieval surfaces distinguish `found`, `empty`, `degraded`, and `failed`.
Healthy empty results and degraded local evidence may include a structured,
prefilled alternative action. The offer is inert data: it carries the required
capability, a one-action advisory budget, `policy_recheck=true`, and
`authorized=false`. Following it always creates an ordinary tool call that must
pass the current toolset, capability, execution-policy, and effect-contract
checks. Failed or unauthorized retrievals do not suggest an external action.

## Checker isolation

Automated reviewers return a typed verdict only: approved, rejected, skipped,
or error. Their prompt, rationale, and model transcript are not control inputs to
the pipeline. Approval and rejection have fixed meanings; skipped/error results
are mapped by an explicit failure mode. The authoring pipeline uses `degrade`,
which escalates to a human after bounded infrastructure retries instead of
silently treating an unavailable checker as assent.

## Benchmark identity

Every stored agent-evaluation row carries an immutable manifest identity:

- a SHA-256 of the suite name and canonical task content;
- a SHA-256 binding the exact Aimee build and selected agent;
- the harness contract version and random seed; and
- a bounded hardware profile (or an operator-pinned profile).

Comparison is a gate, not a label. Quality results require matching dataset,
target, harness, and seed. Latency additionally requires matching hardware.
Missing identity produces `unknown`; a mismatch produces `incomparable`, with a
stable reason code, instead of manufacturing a regression or improvement from
different experiments.

## Context authority

Canonical request blocks carry metadata independently from their wire content:
origin, authority, trust, sensitivity, model visibility, and an optional
knowledge revision. This keeps retrieved or tool-produced evidence distinct
from task instructions and platform policy even when a provider ultimately
renders both as text.

Authority promotion is explicit and conservative. Model, tool, retrieval, and
memory output cannot become a task instruction or policy. User content can be a
task instruction but cannot become platform policy. Code-owned guidance and
freshness notices are platform instructions; recalled material remains
unverified evidence in its own block.

## Knowledge freshness

Knowledge is versioned by a domain and scope identifier. Live curator
invalidations advance both the affected scope and the aggregate knowledge
epoch. Each session records the aggregate epoch it last observed. When a later
turn observes a newer epoch, the request gains a typed instruction to
re-retrieve affected facts rather than silently relying on an earlier answer.

The in-process epoch table is a bounded cache, not the source of truth. The
durable curator feed remains authoritative across daemon restarts. An unseen
session or scope is reported as `unknown`, never incorrectly asserted current.

## Effect contracts

Every tool dispatch can be represented as a mechanical effect proposal: tool
identity, effect class, target digest, normalized-arguments digest, mode, and
lifecycle state. Target and argument content are hashed at the boundary and are
never retained by the contract or emitted in events.

Shadow mode observes `proposed`, `validated` or `mismatch`, `executing`, and a
terminal outcome without changing authorization. It provides a safe measurement
period for contract coverage and drift before enforcement is enabled. Existing
role, execution-policy, workflow, and guardrail decisions remain authoritative.

Reversible enforcement is deliberately narrower than the write-tool category.
It applies only where the dispatcher has a stable target and a mechanical
postcondition: file creation/replacement, string or anchored file edits, and
symbol edits with an explicit path. A contract mismatch or absent target refuses
execution. A nominally successful mutation is not reported successful until a
read-back check passes; failed verification reports an uncertain local state and
requires inspection before retry. Other writes remain in shadow mode.

External effects are identified independently from ordinary read/write labels.
Web access is external but read-only; publication tools and unknown remote tools
are external mutations. Those mutations require a trusted execution-policy
decision carried out-of-band on the dispatch thread. A model cannot assert its
own authorization in tool arguments.

Contracts also declare idempotency. A timeout from a non-idempotent or unknown
external mutation is an `unknown_outcome`, not a clean failure: the operation may
have reached the remote system, so the dispatcher does not imply that retrying is
safe. Automatic retry remains prohibited until reconciliation establishes the
remote state.
