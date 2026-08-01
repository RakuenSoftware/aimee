# E6 paired coding task prompt v1

Work only in the supplied checkout. Diagnose the task, inspect relevant code, make the smallest
correct edit, and run the named verification. Do not use evidence from another cell. When an Aimee
context packet is visible, cite the first consumed result before the decisive edit; otherwise state
that no packet was consumed. Return task success, uncached input tokens, total wall time, and the
first decisive-edit timestamp in the machine-readable result envelope.

The `standard`, `observe`, and `on` arms receive byte-identical task text. `observe` may retrieve but
must preserve standard model-visible bytes. `on` may inject only the bounded product packet. The
`ceiling` arm additionally receives the task's checked-in oracle context after this prompt.
