# Runtime personas and conversational goals

Implemented in the Discord bridge on 2026-10-10:

- Versioned operator persona profiles and exact guild/channel/thread selection.
- Durable scoped goals with objective, explicit evidence milestones, ordered next
  steps, bounded exchanges and operator pause/resume/cancel/complete controls.
- Atomic per-turn snapshots and conditional progress updates after successful delivery.
- Profile changes take effect without restart, preserving human context and dropping
  the old assistant voice. Controls cannot be executed by human or bot chat content.
- Context delivery on the current CPU bot, a private persistent SQLite volume, and
  a native `aimee.behavior.v1` export artifact explicitly marked manifest-only.
- Completion evidence moves goals to review; the operator confirms final completion.

Implementation: `integrations/discord/behavior.py` and `bridge.py`. Operator examples
and commands are in the Discord README. Tests cover isolation, persistence, evidence,
bot/human separation, stale-turn refusal, failed delivery, and persona swaps.

Native attention-bank preparation, authenticated selection and acknowledgement remain
[pending](../pending/native-attention-personas-goals.md), as do shared progress across
multiple scopes and semantic progress proposals. No native attention capability is
claimed by the context delivery path or manifest exporter.
