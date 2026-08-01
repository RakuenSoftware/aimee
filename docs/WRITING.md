# Documentation must make the next decision easier

Completeness is not the hard part. Each claim must be current, sourced, and useful to the reader.
This guide adapts the project voice for reference documentation.

## Lead with the reader's task

State why the page exists before explaining mechanics. Put the common path first, then boundaries,
failure behavior, and recovery. Use short examples that a reader can run or adapt.

When the obvious metric is not the real constraint, name the binding constraint. For example, a
faster deployment is not better if it weakens storage ownership or makes recovery ambiguous.

## Say what is true once

- Prefer verbs, concrete nouns, and short sentences.
- Use `you` for an action the reader takes. Use `we` only for a deliberate project decision.
- Calibrate claims with terms such as `measured`, `estimated`, `expected`, and `unknown`.
- Name the owner, boundary, and failure mode when they affect a decision.
- Replace criticism with a safer command, design, or operating path.

Do not use em dashes, emoji, decorative questions, hype adjectives, or intensifiers. In particular,
avoid `revolutionary`, `game-changing`, `seamless`, `powerful`, `robust`, `very`, `extremely`,
`incredibly`, and `massively`.

## Keep evidence attached to claims

A number without provenance is decoration. Link the checked-in result, fixture, source constant, or
command that produced it. Record the commit, configuration, corpus, hardware, and sample count when
they can change the result. Label examples and deployment estimates as such.

State a disqualifying caveat beside the claim it limits. Do not hide it in a later section.

## Keep current guidance separate from history

Put current commands and behavior in product guides. Keep design decisions under `docs/proposals/`
and point-in-time results under `docs/validation/` or `benchmarks/`. A completed proposal explains
why a decision was made; it does not override the current guide or code.

Files under `docs/gen/` come from source descriptors. Change the source and regenerate them. Never
repair generated Markdown by hand.

## Use visuals only when they carry information

Add a diagram when relationships, ownership, or a sequence is harder to understand in prose. Prefer
SVG for stable architecture and process diagrams because it stays legible at different sizes.

Use a screenshot only when the exact interface matters. Capture it from the current build, remove
credentials and personal data, crop unrelated chrome, give it specific alt text, and update it with
the interface. Decorative images create maintenance work without helping the reader.

Store shared documentation assets under `docs/images/`. Use relative paths so local and hosted
renderers resolve them the same way.

## Keep the documentation navigable

Give each maintained page one H1 and descriptive headings. Link a new public guide from
[`docs/README.md`](README.md). Prefer one canonical explanation and link to it instead of copying a
table or procedure.

Before sending a documentation change, run:

```bash
make -C src docs-gen
python3 scripts/check-docs.py
git diff --check
```

Review the rendered page as well as the source. A link can exist and still point the reader to the
wrong decision.
