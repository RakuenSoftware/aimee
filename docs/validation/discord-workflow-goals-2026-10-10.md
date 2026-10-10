# Discord goals through the Go WFE — 2026-10-10

## Implemented and deployed

CT9211 on .253 now runs `aimee-discord-bridge:paper-quality-20261010-v5`.
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

- 103 Python bridge/controller/preparation/workflow tests pass.
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
The initially delivered model output repeats headings and incorrectly describes Black's
winning queen move as desperate; it also overstates the physical-collapse metaphor.
The subsequent adapter update explicitly supplies correct chess facts, forbids
claiming that discussed examples were actually played, distinguishes metaphor from
physical events, and requests body text without headings. These prompt changes
are not evidence that the initial paper was corrected. Comprehensive semantic
quality checks remain pending.
Readiness and writing quality depend on the model; verbatim source grounding protects
provenance but cannot prove academic or comedic quality. Inference sees bounded
excerpts; complete inputs and artifacts remain durable. The initial adapter supports
explicit paper/essay/article/report goals and rejects unrelated workflow families.
It is intended for the dedicated Discord WFE, not a shared coding engine. Discord
receipts and SQLite cannot form an atomic cross-service transaction; final retry
recovery checks recent channel history. Native attention conditioning remains pending.


## Corrected-paper writing — subsequent repair

The next repair gives sections distinct tasks and removes model-owned headings.
It supplies correct chess facts and treats the illustration as hypothetical.
Explicit false assertions and repeated long sentences trigger one rewrite; a
persistent defect fails the stage before delivery. Full generated section bodies
are inspected. The checks are deliberately narrow and do not prove comprehensive
academic or comic quality.

A model-as-judge prototype correctly rejected “Black loses after Qh4#” but also
rejected a correct hypothetical where White loses and Black wins. It did not
qualify as a production gate. The deployed assertion checks distinguish these,
including locally negated errors, literal versus metaphorical destruction,
unsupported claims of actual games/experiments, and unsupported Samy quotations.

An operator revision helper verifies the accepted source run and artifact hashes,
then atomically saves its candidate/discussion/outline inputs with a new scoped
goal. A new WFE run reuses these artifacts and writes a fresh paper; the original
accepted run and delivered attachment remain intact. Tests cover source corruption,
atomic seed preservation, no repeated discussion messages, bounded quality rewrite,
refusal to deliver persistent errors, redundant headings, long-tail errors,
correct hypotheticals and repeated-sentence repair.


The first corrected run `wi_048b3e62ebebae6fffc8befdecd67c6d` produced a
hypothetical draft with correct Black checkmate, but its final revision retained
literal physical board destruction after one rewrite. The quality check rejected
that run without delivering a paper. The follow-up repair removes obsolete draft
wording from revision prompts, keeps only the agreed argument, requests plain
prose and a concrete imagined scene, strips bare duplicate section labels, and
corrects attribution of White/Black moves. Retrying a failed revision seeds a new
run from the accepted origin; it cannot replace an active run or silently reopen
the rejected one.


The writing retry `wi_b799bf3a2700d0aa5a540acf0e180baf` reached `deliver` /
`accepted` with fresh plain-English prose, an imagined twenty-move-plan joke,
correct Black checkmate and Samy's actual critique. Inspection found one remaining
notation typo: the method mislabeled `e5` as Black's second move. The adapter now
normalizes move numbers only when the text identifies the known f3/e5/g4/Qh4#
example. It leaves quoted collaboration evidence and unrelated variations intact.
An accepted-paper notation repair derives new integrity-checked artifacts through
the WFE without regenerating its already-authored prose. Regression checks cover
that derivation, source preservation and unrelated openings.


Final corrected run `wi_8fd7a5b9141d98015bcdc587a4211af2` reached `deliver` /
`accepted`, with scoped goal status `complete` and all six node artifacts saved.
The body has 960 words. Its method correctly labels Black's move `1... e5`;
`2... e5` no longer appears in the body. Authenticated Discord inspection found
exactly one attachment delivery from the actual bot account for this run. The
downloaded attachment's SHA-256 matches the saved paper artifact. The deployed
adapter source hash matches the PR worktree. No synthetic peer messages or human
approval gate were used.
