# Proposals

Proposals record design decisions and delivery contracts. They are evidence, not the user manual.

## States

- `pending/`: under design, review, implementation, or reconciliation;
- `done/`: implemented and accepted, or preserved as the decision record for shipped work.

A pending proposal can describe code that does not exist. A done proposal can retain names or
constraints that later changed. Current product behavior belongs in the guide for that feature.

## Required shape

A proposal should state:

- problem and boundary;
- decision and non-goals;
- owners and dependencies;
- threat/failure model where relevant;
- compatibility and migration;
- bounded slices;
- mechanical and integration acceptance checks;
- status and supersession links.

Plans may live beside the proposal when execution ordering needs its own artifact. Validation reports
belong under `docs/validation/` and must name the commit, environment, and commands they proved.

## Lifecycle

1. Draft in `pending/`.
2. Review the contract before implementation.
3. Record amendments instead of silently changing an accepted premise.
4. Land slices with the proposal's checks.
5. Reconcile shipped behavior, docs, and unresolved gaps.
6. Move to `done/` when no promised slice remains pending.
7. Link any residual proposal rather than calling partial work complete.

Moving a file does not prove implementation. Acceptance commands and the integrated code do.

## Event-bus records

The detailed event-bus decisions and feature tree preserve the v0 wire, regions, leases, routing,
flow control, capture, conformance, and performance rationale. [Event bus](EVENT_BUS.md) is the
current reader/developer guide; use the design records only when changing those contracts.

## Integrity

`make -C src proposal-links-check` checks links. Reconciliation checks catch stale states and missing
residuals. Keep filenames stable after review so commits, audit rows, and external discussion remain
traceable.

Do not rewrite historical review findings to sound current. Add a dated correction or superseding
record.
