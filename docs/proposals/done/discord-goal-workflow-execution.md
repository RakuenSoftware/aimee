# Discord writing goals execute through the existing Go WFE

Implemented 2026-10-10. Persona/goal conditioning alone did not schedule work:
Aimee could recall the chess-paper objective while continuing unrelated bot banter.
The Discord adapter now submits supported writing goals to the existing Go WFE,
persists the run ID, reports actual stage/state, and routes lifecycle controls to
that run. Stable submission keys and durable stage-result caching survive restart.

The initial workflow is candidates → discussion with verified peers → outline →
draft → revision → delivery. Real Discord contributions are retained with author
and message links. The discussion may span many exchanges: cumulative synthesis and follow-ups are
durable, and grounded readiness requires a candidate, comparison, critique and
resolution. Missing or insufficient input parks the WFE rather than inventing a
conversation or advancing after a fixed number of messages. Lifecycle resume
supports these external input waits while preserving protected human gates. Finished papers are WFE artifacts and Discord attachments.
This kind of goal completes autonomously; it does not require human approval.

The private Unix typed runner executes stage I/O only. Go continues to own workflow
admission, leases, scheduling, lifecycle, and artifact persistence. No additional
bridge goal scheduler or bot turn cap was introduced. Unconfigured or unrelated
workflows are refused by this dedicated adapter.

Live discussion recovery: task inference no longer uses ordinary chat behavior.
Four bounded source-selection tasks preserve early critique and later resolution
across the complete durable discussion. Goal-specific follow-ups prevent unrelated
banter from replacing unfinished requirements. The agreed-argument brief feeds
drafting and revision. Assessment errors use typed stage failures; socket readiness
and one-time startup transport recovery prevent deployment races.

Corrected-paper revisions preserve an accepted run and its collaborator inputs,
then create a new bound WFE run. Per-section tasks, adapter-owned headings,
explicit chess facts, repeated-sentence detection and narrow false-assertion checks
trigger at most one rewrite before failing visibly. A small-model semantic judge
was rejected after it failed a live correct-versus-incorrect-example check.

Remaining broader capabilities: selecting additional bounded workflow families,
semantic quality checks, and sharing one objective across multiple channels. Native
attention-bank delivery remains in its [separate pending proposal](../pending/native-attention-personas-goals.md).
