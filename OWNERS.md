# Ownership

Every durable store, wire contract, and policy boundary has one runtime owner. Shared helpers may
serve several modules; they do not inherit authority over those modules.

`@JBailes` is the current primary repository owner. Before a production promotion, Engineering
Governance must assign and record a second qualified GitHub identity as backup/reviewer in the live
ruleset. A boundary owner may author or advise a change, but the recorded approving reviewer must
be a different identity. Quarterly ruleset/access exports are retained under
`docs/compliance/CONTROL_EVIDENCE.md`; an absent backup or self-approval blocks promotion.

| Area | Owner | Paths | Review focus |
| --- | --- | --- | --- |
| event bus | runtime core | `src/core/event_bus/`, `server-go/bus/` | wire vectors, ordering, backpressure, leases, admission, shutdown |
| audit and governance | audit module | `src/modules/audit/` | completeness, PII bounds, WORM parity, witness behavior |
| DB1 | server | `src/db1/` | migrations, transaction ownership, local privacy |
| DB2 | KB | `src/modules/db2/c/`, `src/kb/` | schema, scope, retrieval, pgvector, ingest |
| workflow lifecycle | Go WFE | `server-go/internal/` | single writer, durable transition before dispatch, recovery, forge confinement |
| tool execution | server tools/policy | tool and guardrail modules | schemas, capabilities, worktree/path checks, audit |
| delegate sandbox | sandbox module | `src/modules/sandbox/`, delegate backends | mounts, network, credentials, packages, resource bounds |
| vault | vault module | vault and credential bridges | custody, principal, rotation, no plaintext fallback |
| provider IR | IR/translation modules | provider ingress/egress modules | canonical shape, loss, tool parity, streaming |
| routes | owning service | route descriptors, OpenAPI, handlers | auth, scope, write tier, compatibility |
| configuration | config module | field descriptors and stores | one descriptor, defaults, validation, restart behavior |
| thin client | client transport | CLI and native TLS files | no database linkage, path ownership, trust state |
| browser | runtime web and frontend | `runtime-web/`, `frontend/` | user isolation, CSRF, proxy authority, no direct storage |

## Boundary rules

- Lower-level helpers cannot include a higher-level owner to get at its state.
- Code outside DB1 or DB2 uses the public typed API, never a database handle.
- A migrated workflow family has one writer. Shadow reads may compare; dual writes are forbidden.
- Provider wire formats end at translation. Core stages consume canonical IR.
- A new inter-module path uses the event bus or an existing typed resource API, not a private queue.
- A new command, route, config field, event kind, or workflow block updates its canonical descriptor
  and generated documentation.

## Required review

Changes need explicit boundary review when they touch:

- event framing, capture, or audit durability;
- storage schema or ownership;
- authentication, write grants, vault custody, or egress;
- tool execution, worktree confinement, or sandbox degradation;
- workflow state transitions, forge operations, or human gates;
- public API compatibility or generated SDKs;
- cross-language C/Go contracts.

Run the narrow unit target while iterating, then the ownership, linkage, module, docs, and sanitizer
gates named by the changed area.
