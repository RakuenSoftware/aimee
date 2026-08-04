# Code graph route, storage, and orientation: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`code-graph-architecture-surface.md`](../done/code-graph-architecture-surface.md)

## Delivered baseline

The graph now carries project lifecycle, architectural diff, scan provenance and confidence,
task-conditioned context, and language-aware blast-radius evidence.

## Remaining deliverables

- Add first-class route and storage node kinds with `handles` and read/write `touches` edges.
- Extract those nodes for the repository's supported framework idioms and report blind spots.
- Produce a compact repository orientation with surface counts, cycles, depth outliers, and hotspots.
- Inject bounded orientation at session start from indexed evidence rather than a filesystem rescan.
- Group architectural impact by distance and criticality across route/storage edges.

## Completion evidence

Deterministic fixtures must detect representative routes and stores, report unsupported idioms as
coverage gaps, and show session-start orientation plus blast radius without live source scanning.
