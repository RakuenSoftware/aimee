# Conversational memory and human interaction

- **State:** pending.
- **Scope:** durable, useful conversational memory for Aimee and the Discord bridge, independent of the selected retrieval backend. Bot-to-bot conversations remain unlimited, as requested for chaos/behavior testing.

## Current gap and intended behavior

The bridge's live bot-chat and height correction fixes demonstrate conversation and exact-unit/source repair, but not a general conversational memory solution. The bridge has a short volatile history, archives turns separately from shared knowledge, uses a narrow lexical search and small custom context packer, and does not build a durable conversation projection or request the implemented served-memory views each turn. Its generated reply can influence whether an admitted human statement is captured. Existing MR-12 views, MR-13 projections and MR-04 lineage provide the foundation; inventing another canonical memory engine or another set of height regexes would not close these gaps.

## Deliverables

1. Durable, speaker-attributed episodes and a rebuildable conversation projection: current purpose, partners, open questions, decisions and latest human constraints. Recover after restart, pronoun follow-up, topic changes and “what did we agree?”; recall original spans on request. Bound storage/context by explicit policy without limiting bot conversation turns.
2. Distinguish speaker claims from verified facts, self-reports, third-party reports, quotes, jokes, fiction and test assertions. Capture authenticated speaker/message identity and source relation. A bot restating a human claim is not independent support. Preserve uncertainty in normal language and update current/historical claims on corrections before inference. Exact units apply to every measurement domain.
3. Backend-neutral intent-aware context assembly using served views, episodes, claim cards and raw-source fallback. Prefer memories useful for the current reply; attach coverage/omission receipts and enforce the final token budget rather than a fixed 300-byte excerpt. Social chat should flow naturally; attribution and remembering requests should retrieve the relevant evidence.
4. Capture eligible human input independently of generated wording, before generation or through the existing durable jobs with visible completion/failure. Bot turns may enrich episodes without being promoted to world facts. Separate remembering failure from ordinary conversational ability; never invent a remembered source during an outage.
5. Provenance-bound human feedback and scoped preferences with explicit lifetime. Confirmed failures become reproducible trajectories. Feedback affects answer behavior without turning bot agreement, repetition or engagement into truth or reward.

## Acceptance

Freeze multi-speaker conversational trajectories for: 69cm versus feet; mistaken human/mountain entity links; “who told you?”; conflicting reporters; jokes/roleplay then real correction; pronoun follow-ups; explicit preferences and expiry; topic return/restart; deleted/private memories; backend outage; and an unbounded multi-bot exchange. Compare before/after exact answers, source attribution, unsupported claims, useful context, latency and token cost. Run the same cases against native, Cognee and Hillock profiles with backend limits declared. Never claim memory benefit from retrieval counts alone or auto-promote a learned policy from this fixture.

Related producer work remains in [auto-population](memory-auto-population-phase4.md), while fitted ranking and activation are owned by [ranking/feedback](retrieval-ranking-feedback-and-promotion.md) and [MR promotion residuals](memory-reliability-promotion-and-adapter-residuals.md).
