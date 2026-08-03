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

## Current WFE boundary

`server-go/` is now the sole workflow lifecycle owner. It provides:

- **Durable state:** Go-owned DB1 workflow rows and append-only lifecycle events use a CGO-free
  SQLite driver.
- **Distinct artifacts:** immutable proposals remain separate from atomic plans and feedback.
- **Typed dispatch:** full-size plan and review packets cross an out-of-process runner boundary.
- **Scheduling:** the Go loader, state machine, and scheduler refill available slots immediately.
- **Recoverable loops:** `convergence_limit` and `convergence_no_progress` preserve exhausted work.
- **Worker isolation:** a lost runner retries through a bounded failure path.
- **Compatible APIs:** work-item, event, proposal, and trigger reads retain their `/v1` shapes.
- **Deterministic admission:** Git blobs are read from a refreshed ref and deduplicated by content.

The Go process does not link the C server. Native execution uses typed agent and forge resource
calls. A configured compatibility runner remains disposable: its death is a failed attempt, not a
control-plane failure. The C workflow engine and scheduler no longer start.

## Migration sequence

1. **Retire the remaining C WFE inventory.** Move reusable CLI and resource helpers behind Go APIs,
   then delete lifecycle code that no supported route reaches.
2. **Keep one WFE writer.** Go owns workflow tables, definitions, artifacts, scheduling, and recovery.
3. **Move remaining DB1 families.** Transfer session and authentication control one
   family at a time, maintaining one writer per family.
4. **Isolate resource execution.** Move delegate/provider execution, vault, and policy enforcement into
   Go services. Preserve durable admission-before-dispatch semantics.
5. **Rewrite KB ownership.** Move the KB service and DB2 while retaining typed HTTP boundaries.
6. **Replace remaining clients.** Move thin-client and gateway behavior where needed. Delete compatibility
   adapters once no deployed client depends on them.
7. **Remove native runtime dependencies.** Any retained
   native component requires a documented Go impossibility and an isolated Rust
   boundary.

## Verification gates

Every migrated family must prove:

- **Concurrency:** race-enabled unit and integration tests.
- **Recovery:** restart from every durable transition.
- **Isolation:** worker kill and connection-loss tests.
- **Artifact integrity:** exact multi-megabyte Unicode round trips, including final-byte
  acceptance criteria and blocker recommendations.
- **Compatibility:** API fixtures against the current client.
- **Ownership:** single-writer enforcement during migration.
- **Delivery:** an end-to-end proposal commit produces a PR without a C control-plane process.
