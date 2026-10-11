# Runtime persona and goal validation — 2026-10-10

62 bridge, model-preparation and behavior-controller tests passed. Coverage includes
profile versioning and scope isolation, restart persistence, bot/human evidence
separation, explicit review and operator completion, pause/replacement races,
bounded exchange records, prompt budgeting, persona-history removal, and no goal
progress on failed public delivery.

Deployed to CT 9211 at 192.168.1.253 as
`aimee-discord-bridge:behavior-20261010`, using a private UID-1000 persistent volume.
The current English concierge default remains selected; no live goal was assigned.
Other services were not changed.

Read-only live model probes selected concierge and haunted-librarian snapshots in
one process, with a scoped business objective. Responses reflected the selected
voices but did not reliably produce the requested pitch. Goal-directed quality is
not certified by these small-model probes. A final prompt reminder explicitly asks
for concrete results rather than letting persona humor replace the goal task.

An isolated validation scope exercised human milestone evidence, assistant evidence,
review status, and exclusion of review goals from the attention manifest. Its profile,
goal, progress and status survived a container restart. The fixture scope and profiles
were then deleted; the real channel's default profile and absence of goal were verified.
No validation conversations were sent to Discord or admitted to factual memory.

Native delivery is a manifest-only handoff. The current model-plugin broker prepares
memory with a serving-level `native_system_context`; it has no qualified request-scoped
persona/goal-bank selector. Remaining plugin requirements are recorded in
`docs/proposals/pending/native-attention-personas-goals.md`. Context conditioning is
not represented as native attention injection.

The final live goal probe, after the explicit next-step reminder, produced an English
pitch beginning `Our pitch: Boredom Umbrellas are a revolutionary accessory designed
to repel the mundane chatter that plagues polite society.` This confirms one working
goal-directed generation, not general goal-completion quality. Deployed source hashes
matched the tested bridge and controller; the container ran with zero restarts after
final recreation. Operator profiles `concierge` and `librarian` are available, while
the actual channel remains on its configured default and has no active goal.
