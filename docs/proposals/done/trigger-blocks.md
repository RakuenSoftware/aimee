# Triggers as workflow blocks — graph-native run starts

## Why

Triggers today live *outside* the workflow system: a `trigger_rules` list in
`aimee.yaml` names a source (`watch-dir`/`proposals`, `cron`) and points it at a
saved workflow. That works — the proposals pipeline files and completes runs
hands-off — but it splits one design across two authoring surfaces. The
workflow graph declares everything about a run *except* what starts it; the
thing that starts it is a config stanza the composer never sees, validated by
different code, with its own field vocabulary (`event`, `schedule`,
`pipeline.workspace`) that overloads meanings per source.

The trigger-source registry (`{name, due, fire}` in
`src/server/trigger_scheduler.c`) already made sources pluggable. This proposal
finishes the thought: a trigger becomes a **block** — the entry node of the
workflow that wants it — so "what starts this workflow" is composed, validated,
versioned, and displayed exactly like every other step.

## What

1. **A `trigger.*` block kind** in the catalog (`wfe_def.c`), starting with one
   concrete block:
   - `trigger.watch-dir` — params `{dir, ref, suffix}` (defaults
     `docs/proposals/pending`, auto-detected origin HEAD, `.md`). Produces a
     `proposal` artifact: the materialized content of each new file seen in the
     watched directory, exactly what the `watch-dir` source materializes today.
   - The block-kind namespace leaves room for `trigger.cron` and
     `trigger.webhook` rows later; each is a thin adapter over the same
     scheduler plumbing.
2. **Validator rules**: a `trigger.*` block may appear only as the `start`
   node, at most one per workflow, with no inbound edges and no `in` bindings.
   Its `next` edge feeds the produced artifact into the graph (e.g.
   `trigger.watch-dir → author.proposal` replaces the pre-supplied-proposal
   special case with an explicit data edge).
3. **Arming**: a saved workflow whose start node is a trigger block is an
   *armed* workflow. The trigger scheduler's tick enumerates armed workflows
   (same registry, same per-rule rate limit, same global
   `trigger.max_concurrent`, same per-run USD ceiling policy) and instantiates
   one work item per event through the existing `trigger_file_run` back half —
   the run starts at the node *after* the trigger block, with the trigger's
   artifact bound.
4. **Workspace binding**: the armed workflow carries its repo binding
   (`params.workspace` on the trigger block, or the project the workflow was
   saved under in the webchat). Executors already honor the work item's own
   repo (`wfe_repo_local`), so this closes the loop: one YAML file states
   *watch this dir, in this repo, and run this graph*.
5. **Compatibility**: `trigger_rules` keeps working unchanged — it is the
   "arm someone else's workflow without editing it" form, and its sources and
   the trigger blocks share one implementation. The webchat composer gains the
   trigger blocks in the rail like any other block.

## Acceptance

- `aimee workflow validate` accepts a workflow whose start is
  `trigger.watch-dir` and rejects a trigger block anywhere else in the graph
  (non-start, inbound edge, `in` binding, or a second trigger).
- Saving such a workflow arms it: committing a matching file into the watched
  dir files exactly one run bound to the configured repo, with the standard
  cost cap, visible in the Workflow Actions tab; re-scans never double-file.
- Disabling/deleting the workflow disarms it (no orphaned watchers).
- An equivalent `trigger_rules` stanza and an armed workflow produce
  byte-identical work items (same intake, cap, audit events) for the same
  event.
- The `build` composition gains a sibling `build-triggered.yaml` example whose
  entry is `trigger.watch-dir`, and `docs/WORKFLOWS.md` documents the block
  form as the primary authoring path with `trigger_rules` as the external
  alternative.
