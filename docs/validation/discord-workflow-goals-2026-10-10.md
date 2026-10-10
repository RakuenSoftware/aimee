# Discord goals through the Go WFE — 2026-10-10

## Implemented and deployed

CT9211 on .253 now runs `aimee-discord-bridge:discussion-recovery-20261010-v2`.
Its dedicated bot server runs `aimee-discord-server:conversation-waits-20261010-v2`.
The existing WFE executes a pinned `discord-paper` graph through its typed runner
on a private Unix socket. It owns admission, scheduling, lifecycle, and artifacts.
The adapter's watcher only mirrors status and releases input waits when the
configured collaborator provides a new contribution. No human gate or fixed
conversation turn limit exists in this graph.

The genuine saved goal, “Write an academic paper on the funniest way of losing at
chess with Samy,” is bound to run `wi_94fcbcd6ff9bd442a238201c9f5e27b6`.
The deployed registry accepted graph version
`8242caebc539b11455ed95e40b690000d9ad8923630fc7a6f8cdfa928687e20b`.
The bot identity for Samy was verified using authenticated Discord identity data.
Aimee saved candidate analysis, asked Samy for critique, ingested his actual
responses, and sent a substantive follow-up rather than advancing on the first
reply. The same run, artifacts and inputs survived adapter/container replacements.
No synthetic messages were posted to the public channel.

The first live input wait exposed a contract gap: DB1 refused to resume the new
`conversation_input` reason. The lifecycle owner now permits retries of this and
`binding_pending`, while continuing to refuse protected human gates and child
waits. Runner retries revalidate scope, run, stage and required input; retrying a
conversation wait cannot itself skip the discussion. The packaged owner executable
was updated at its actual libexec path and the real run resumed through the WFE API.

## Checks

- 94 Python bridge/controller/preparation/workflow tests pass.
- Go suites pass for `modules/aimee/families`, `internal/wfe`, `internal/engine`,
  `internal/api`, and `internal/workflowstore`.
- Typed Unix transport, stable submission keys, durable stage replay, authoritative
  lifecycle state, full artifact preservation, and failed-send recovery are covered.
- A discussion continues across twelve unresolved test exchanges and recovers its
  cumulative state. Unverified peers and fabricated readiness quotes cannot close it.
- A cancelled in-flight discussion cannot send its generated late follow-up.
- The final typed stage chain saves/delivers a complete artifact and finishes without
  a human gate. A previous bot delivery receipt prevents resend; a human spoof does not.
- Actual CPU inference produced a valid structured discussion assessment using Samy's
  saved response, identifying unresolved humor analysis and proposing a concrete reply.

## Live recovery and completion

The original discussion accumulated 23 peer inputs and 20 follow-ups before an
assessment error became HTTP 400 and parked the stage as `delegate_failed`.
Task inference now bypasses ordinary chat persona/retrieval, uses four bounded
source selections with one validation repair each, and retains earlier critique
and post-follow-up resolution despite later banter. The bridge supplies actual
source text; model parse errors become typed stage failures. Goal-specific
follow-ups and an agreed-argument brief keep subsequent writing on the objective.

The same genuine run resumed, saved discussion, outline, draft and revision,
delivered the paper attachment, and reached `deliver` / `accepted`. Scoped goal
status is `complete`; all six node artifacts remain saved. Its revised body has
2,172 words plus the complete collaboration evidence. No synthetic Discord inputs,
fixed conversation turn cap, or human approval gate were used to achieve this.

Deployment initially exposed a socket-startup race. The new health endpoint waits
for Discord readiness; deployment resumes only after it reports ready. Startup
recovery retries a confirmed Unix socket connection failure once per run. Genuine
assessment failures remain visible. Regression checks cover these distinctions,
early critique recovery after thirty drifting messages, invalid evidence choices,
bounded repair, task inference isolation and evidence carried into writing.

## Limits

Live execution and delivery are verified; paper quality is not fully qualified.
The delivered model output repeats headings and incorrectly describes Black's
winning queen move as desperate; it also overstates the physical-collapse metaphor.
The subsequent adapter update explicitly supplies correct chess facts, forbids
claiming that discussed examples were actually played, distinguishes metaphor from
physical events, and requests body text without headings. These prompt changes
are not evidence that the already delivered paper was corrected. Automated semantic
quality checks remain pending.
Readiness and writing quality depend on the model; verbatim source grounding protects
provenance but cannot prove academic or comedic quality. Inference sees bounded
excerpts; complete inputs and artifacts remain durable. The initial adapter supports
explicit paper/essay/article/report goals and rejects unrelated workflow families.
It is intended for the dedicated Discord WFE, not a shared coding engine. Discord
receipts and SQLite cannot form an atomic cross-service transaction; final retry
recovery checks recent channel history. Native attention conditioning remains pending.
