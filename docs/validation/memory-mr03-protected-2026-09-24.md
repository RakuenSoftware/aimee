# MR-03 protected projections — 2026-09-24

MR-03 remains active until final image and process verification completes. Its
[six-gate checklist](../proposals/pending/memory-reliability-03-closeout.md) preserves
the original acceptance clauses.

The fold regression reproduced loss of user, system and developer messages when
their constraints followed a long introduction. Folding now copies those
messages verbatim and in order, summarizing only optional history as assistant
evidence. Model-authored labels cannot create protected classes. Mixed user
text/tool-result messages remain whole; folding refuses a boundary that would
orphan their tool call. Compression also preserves the entire mixed message.

The composed reducer compares protected messages exactly, rejecting changed
negation, numerical limits, deadlines, identifiers, role changes and inserted
user instructions. Native delegate reduction now applies the gateway's existing
shrink and tool-pair admission before returning a candidate. Original-payload
fallback still passes the final serialized-request byte gate. Legacy bytes/4
estimates are forecasts, not proof of hard token compliance or exact cost.

Folded history and generated recall/identifier notices use assistant provenance.
Cache tests now compare the entire folded prefix, not only its first protected
message. The fold golden intentionally changes from the original C projection;
its protected roles, order and complete bytes are independently asserted.

Two packaged-runtime regressions reproduced automatic promotion of repeated
anti-pattern observations into hard rules and model extraction overwriting an
existing hard rule. Escalation now emits soft guidance while preserving threshold
and deduplication behavior. Model extraction updates only soft guidance; a hard
rule with the same title prevents both replacement and a duplicate soft rule.
Existing hard rules are retained without guessing their historical authorship.
No schema migration is introduced.

## Component evidence

- [Protected-message red regression](memory-mr03-protected-2026-09-24/fold-red.txt).
- [Automatic rule-promotion red regression](memory-mr03-protected-2026-09-24/rule-promotion-red.txt).
- [Protected rule-rewrite red regression](memory-mr03-protected-2026-09-24/rule-rewrite-red.txt).
- [Economizer race suite](memory-mr03-protected-2026-09-24/economizer-race.txt): passed in 3.797 seconds.
- [77 lint gates](memory-mr03-protected-2026-09-24/lint.txt): passed.

[Full memory race/export](memory-mr03-protected-2026-09-24/memory-race-export.txt)
passed in 300.010 seconds, with exported-owner verification in 5.122 seconds.
Final image/process verification remains pending. The new
native fixture forces a real fold through five tool calls and independently
checks provider-visible protected bytes and summary provenance. Provider fixtures
also exercise literal-zero token caps and token reserves, which must refuse
explicitly when provider-bound counting is unavailable.

## Unobservable external backends

The final route audit found that tmux/provider-CLI execution can occur before
native HTTP serialization. Those external backends cannot attest their eventual
provider request size. Their native-run and direct compute entry points now
refuse caller or operator hard limits with `request_budget_unavailable`. Existing
context refusals also survive this routing boundary. Native HTTP adapters still
reach normal final-body admission; unbounded external calls retain their prior
behavior. [Native fence tests](memory-mr03-protected-2026-09-24/native-fence.txt)
cover absent, malformed, literal-present and inherited limits. The live fixture
uses `/bin/false` as its external command so even a regression cannot contact an
external model provider.

Generated evidence notices remain within the retained tail, inserted before a
complete user turn or tool cycle. They cannot change the final provider role,
invent an assistant prefill, split a tool pair or enter the frozen prefix. If no
safe slot exists, compression retains the original. The strengthened
[economizer race suite](memory-mr03-protected-2026-09-24/economizer-tail-race.txt)
passed in 3.672 seconds.
