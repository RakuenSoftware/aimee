# Go-first server rewrite

## Decision

Aimee is being rewritten as Go-owned services. C is not an ownership boundary in
the target architecture. During migration, a C process may exist only as a
stateless compatibility adapter or an isolated legacy worker; it must not remain
authoritative for state, scheduling, artifacts, credentials, policy, or recovery.

Rust is used only when a required capability cannot physically be implemented at
the necessary boundary in Go. Parsing, orchestration, HTTP, persistence,
scheduling, provider clients, process supervision, Git operations, and artifact
storage are Go responsibilities. Familiarity, speculative performance, or a
preference for another language is not sufficient justification for Rust.

## Non-negotiable contracts

1. **No silent content truncation.** Text and structured artifacts use dynamic,
   length-aware values. A component either transfers and validates the complete
   value or returns an explicit error that parks the workflow. A byte prefix is
   never treated as a valid proposal, plan, review, prompt, response, or diff.
2. **Immutable request, distinct products.** The admitted proposal/request is
   immutable. Plans, implementation output, documentation, and review feedback
   are separate versioned artifacts.
3. **Recoverable convergence.** A loop ceiling and repeated identical
   plan/feedback fingerprints park a run with a diagnostic. They do not convert
   an autonomous run into terminal abandonment.
4. **Crash isolation.** The Go control plane owns durable workflow state before
   dispatching external work. A provider, model, tool, or transitional worker
   crash cannot corrupt that state or terminate the server. Transient worker
   loss is retried after backoff.
5. **One authority during cutover.** A migrated table, API family, scheduler, or
   artifact namespace has exactly one writer. Dual-write is prohibited. Reads
   may temporarily compare old and new implementations during shadow validation.
6. **Compatibility is an interface, not ownership.** Existing clients retain the
   `/v1` contract while handlers move behind it. Compatibility adapters are
   removable and hold no durable state.

## First vertical slice: WFE

`server-go/` establishes the replacement process with:

- Go-owned DB1 workflow rows and append-only lifecycle events using a CGO-free
  SQLite driver;
- immutable proposal storage and independent atomic plan/feedback artifacts;
- full-size typed plan/review packets and out-of-process runner requests;
- a Go workflow definition loader, state machine, scheduler, and immediate slot
  refill;
- recoverable `convergence_limit` and `convergence_no_progress` parks;
- automatic retry after an isolated runner disappears;
- compatible workflow item/event/proposal reads and proposal trigger filing;
- Git-blob admission from a pinned commit with deterministic deduplication.

The vertical slice deliberately does not link the C server. Until a Go runner
implements every production block, the Go control plane may call an isolated
compatibility worker over the typed runner protocol. That worker is disposable:
its death is a failed attempt, not a control-plane failure.

## Migration sequence

1. Complete WFE block runners, roundtables, Git forge operations, worktree
   isolation, and provider routing in Go.
2. Shadow-read current DB1 and compare API responses; stop the C scheduler, then
   transfer the workflow tables to the Go writer in one cutover.
3. Move the remaining DB1 API families and session/auth control plane to Go one
   family at a time, maintaining one writer per family.
4. Move delegate/provider execution, vault and policy enforcement into isolated
   Go services. Preserve durable admission-before-dispatch semantics.
5. Rewrite the KB service and DB2 ownership in Go, retaining typed HTTP boundaries.
6. Replace thin-client and gateway behavior where needed. Delete compatibility
   adapters once no deployed client depends on them.
7. Remove C build/runtime dependencies from production images. Any retained
   native component requires a documented Go impossibility and an isolated Rust
   boundary.

## Verification gates

Every migrated family must prove:

- race-enabled unit and integration tests;
- restart recovery from every durable transition;
- worker kill/connection-loss isolation;
- exact multi-megabyte Unicode artifact round trips, including final-byte
  acceptance criteria and blocker recommendations;
- API compatibility fixtures against the current client;
- single-writer enforcement during migration;
- an end-to-end proposal commit producing an implementation PR without a C
  control-plane process.
