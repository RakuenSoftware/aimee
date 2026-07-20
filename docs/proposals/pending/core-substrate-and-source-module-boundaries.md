# Proposal: make Aimee's core explicit, delete unused complexity, and modularize the source tree

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Author:** Aimee project
- **Date:** 2026-07-20
- **Scope:** source ownership, dependency direction, build composition, optional-module
  configuration, and a substantial internal simplification. Public contracts remain compatible
  unless separately approved, but internal APIs and implementations are expected to be refactored,
  consolidated, and deleted aggressively.
- **Related:** `ir-sole-path-and-pluggable-stages.md` owns the live IR-path conversion and IR-stage
  semantics. This proposal owns the larger source-tree boundary and makes that registry one of the
  core extension surfaces rather than defining a competing stage system.

## Thesis

Aimee is the shared intelligence substrate between agents, models, tools, and applications. Its
irreducible core is:

1. **Memory** — persistent, episodic, working, semantic, and code memory, including the embedding
   and reranking inference required to store and recall it. Code intelligence is not a separate
   feature beside memory: symbols, references, call graphs, dependency graphs, code history,
   architecture facts, and blast radius are memory over code.
2. **Routing** — selecting the right model, agent, tool, workflow, memory source, or destination for
   a typed message.
3. **IR messaging** — the canonical request, response, event, tool, and stream-delta representation
   every participant can share.
4. **Translation** — the contracts and adapters that translate between the IR and provider, agent,
   tool, application, and transport protocols.

Everything else is a module built on that substrate. A module may ship with Aimee and be enabled by
default, but that does not make it part of the core. Delegates, workflows, roundtables, guardrails,
governance, git/forge integration, CSS analysis, sandboxes, dashboards, web-user runtime, skills,
roadmaps, evals, economization, synthesis, and particular delivery channels must be independently
configurable and removable wherever their dependencies permit. Synthesis may enrich or compose an
answer from recalled material, but memory storage and ranked recall must work without it.

The architectural rule is:

> Modules depend on Aimee Core. Aimee Core never depends on an optional module.

The physical tree does not express that rule today. `src/modules/` is already the established
directory-as-module pattern and contains substantial extractions, but feature implementation
remains spread across the root of `src/`, `src/server/`, `src/kb/`, `src/db1/`, `src/db2/`, and the
global `src/headers/` bucket. The build lists then reassemble those files into broad historical
libraries (`CORE_SRCS`, `DATA_SRCS`, `AGENT_SRCS`, `CMD_SRCS`, and others), obscuring ownership and
allowing dependencies to point in either direction.

This proposal finishes the extraction: put implementation in its owning `src/modules/<name>/`
directory, make the four core capabilities explicit mandatory modules, make optional modules
actually selectable, and reduce non-module code to composition roots, small shared primitives,
platform shims, generated sources, tests, and vendored code. It is also a cleanup program: remove
parallel implementations, dead fallbacks, repeated registries and build lists, pass-through
wrappers, oversized mixed-responsibility files, and abstractions that do not earn their cost.

The intended result is not the same amount of code in tidier directories. It is a smaller system
with fewer concepts and one authoritative path for each operation.

## Current state and evidence

As of 2026-07-20, a mechanical inventory finds:

| Location | C/header files | What it indicates |
|---|---:|---|
| `src/modules/` | 417 | The target pattern is already real and widely used. |
| `src/` root | 225 | Large unowned feature surface: commands, code indexing, plugins, sessions, models, dashboards, and utilities. |
| `src/server/` | 186 | Process composition is mixed with agent runtime, routing, providers, IR, translation, tools, and feature handlers. |
| `src/kb/` | 163 | The shared-memory service host is mixed with memory algorithms, code intelligence, curation, ranking, and provider adapters. |
| `src/db1/` | 117 | Personal-memory persistence is organized by database rather than memory ownership. |
| `src/db2/` | 233 | Shared-memory and code-memory persistence is likewise organized by database. |
| `src/headers/` | 266 | Public, private, and cross-module headers share one global namespace. |

The counts are not success metrics by themselves; they establish that this is a broad ownership
repair, not a handful of cosmetic moves. The existing `src/modules/memory/`, `config/`,
`delegates/`, `workflows/`, `roundtable/`, `git/`, `audit/`, `guardrails/`, `kb_client/`, `vault/`,
and protocol directories demonstrate the intended destination.

## Terminology: core module, optional module, plugin

These concepts must not be conflated:

- A **source module** is a directory with one owner, a public contract, private implementation,
  declared dependencies, tests, build metadata, and complete module documentation.
- A **core module** is a source module required in every Aimee profile. For v1 these are `memory`,
  `routing`, `ir`, and `translation`.
- An **optional built-in module** is compiled or omitted by a build profile and enabled or disabled
  by configuration when present.
- A **plugin** is an externally discoverable extension using the plugin ABI. Built-in modules and
  plugins may use the same registration contracts, but moving a file under `src/modules/` does not
  require converting it into a dynamically loaded plugin.

This distinction lets the tree become modular before every capability is safe to unload at
runtime. Physical ownership lands first; compile-time and runtime optionality follow behind tested
seams.

## Target source tree

```text
src/
  app/
    cli/                 # thin composition root and generic dispatch only
    server/              # server entry point, listener, lifecycle, composition
    kb/                  # shared-memory service entry point and composition
    gateway/             # optional gateway entry point and composition
  base/                  # tiny dependency-free primitives used by multiple core modules
  modules/
    memory/              # REQUIRED: all memory, including code intelligence
      code/              # symbols, extractors, graph, architecture, blast radius
      inference/
        embedding/       # REQUIRED: vectorization contract and working implementation
        reranking/       # REQUIRED: relevance scoring contract and working implementation
      personal/          # DB1-backed personal/session/working memory adapters
      shared/            # DB2-backed corpus/team memory adapters
      storage/           # persistence contracts and concrete backends
      cli/               # memory/index/graph command adapters
      service/           # memory HTTP/RPC handlers; no listener ownership
    routing/             # REQUIRED: route policy, catalogs, capability selection
    ir/                  # REQUIRED: canonical messages, events, deltas, ownership
    translation/         # REQUIRED: adapter registry and translation contracts
      providers/         # OpenAI, Anthropic, Responses, Bedrock, local, CLI adapters
      protocols/         # MCP, ACP, HTTP/SSE and other IR boundary adapters
    extensions/          # plugin discovery, manifests, registration, lifecycle
    config/
    delegates/
    workflows/
    roundtable/
    synthesis/           # OPTIONAL: generation/composition over recalled memory
    guardrails/
    governance/
    git/
    ...                  # other optional built-ins, one capability per directory
  platform/              # POSIX/Linux/macOS/Windows implementations of base contracts
  generated/             # generated route/schema/help/tool data
  vendor/
  tests/
  Makefile
  README.md
```

The exact subdirectory spelling may change during implementation when an existing, cleaner
boundary already exists. The invariants matter more than the illustration:

- Feature implementations do not live at `src/` root.
- Process directories contain composition and transport shells, not feature logic.
- Headers live with their owner. A module exposes a narrow `include/` contract and keeps other
  headers private.
- Database choice does not own memory semantics. DB1 and DB2 become persistence adapters under
  memory-owned boundaries; migration files may remain deployment artifacts.
- Commands and HTTP/RPC handlers are adapters owned by the feature they expose, not reasons to
  place the feature in a global `cmd_*` or server-state bucket.

## Cleanup principles: less is more

Modularization is the forcing function for a significant refactor. Every migrated family must be
made smaller and clearer where evidence supports it:

- **One path:** choose one canonical implementation, migrate callers, then delete the fallback,
  shadow copy, compatibility mapper, and duplicate data model when its evidence gate is satisfied.
- **One owner:** a concept, registry, schema descriptor, route description, or source list has one
  authority. Other representations are generated or adapted from it rather than hand-maintained.
- **One narrow contract:** modules expose capabilities, not their internal structs. Replace chains
  of pass-through wrappers with a direct public operation or a typed registry call.
- **Delete before abstracting:** remove unused code, stale flags, obsolete shims, and duplicate
  helpers before introducing a new interface around them.
- **Split by responsibility, merge by sameness:** split files that mix routing, translation,
  transport, and feature policy; merge helpers and implementations that perform the same job under
  different names.
- **Prefer data over branching:** generated module/build/route/config descriptors replace repeated
  switch tables and source lists where one declarative authority can serve all consumers.
- **No speculative framework:** a new abstraction needs at least two real consumers in the slice
  that introduces it. Otherwise keep the direct implementation.
- **Bounded core-contract exception:** the first Slice 1 contracts for `memory`, `routing`, `ir`,
  and `translation` are the architectural commitment this program is explicitly making and are
  exempt from the two-consumer rule. Additions to those contracts after Slice 1 require two real
  consumers. If both consumers of any other new abstraction are later deleted, the same slice must
  re-evaluate and normally delete that abstraction.
- **Measure deletion:** every slice carries a deletion/consolidation ledger and reports production
  files, lines, duplicate paths, feature flags, and registries removed or added. Tests and generated
  outputs are reported separately so they cannot disguise production growth.

Significant internal refactoring is explicitly in scope. Compatibility is required at Aimee's
public boundaries—CLI, HTTP/RPC, IR contract, persisted data, config, and supported plugin ABI—not
for private functions, internal headers, historical source paths, or redundant internal types.

## Core contracts

### Memory is one core capability with multiple modalities

The memory module owns the common identity, scope, provenance, relation, retrieval, lifecycle,
confidence, and recall contracts for:

- semantic memory: facts, preferences, decisions, lessons, directives;
- episodic memory: sessions, attempts, actions, outcomes, and history;
- working memory: current-task and temporary state;
- code memory: files, symbols, references, calls, imports, routes, storage touches, architecture,
  cross-repo relations, graph health, and blast radius.

Embedding and reranking are core memory operations, not optional inference features:

- **Embedding** gives memory a shared semantic coordinate system at write/index and query time.
  Its contract, model metadata and dimension checks, provider selection, execution path, and at
  least one supported implementation are present in every core profile.
- **Reranking** turns broad candidate retrieval into useful recall. Its contract, score semantics,
  provider selection, execution path, and at least one supported implementation are likewise
  present in every core profile.
- A deployment may select replaceable local, accelerated, or remote implementations and register
  additional providers, but it may not report a healthy memory service without a resolved embedder
  and reranker. Their absence is a startup/configuration error, not an optional degraded state.
- Embedding and reranking providers register through memory-owned typed registries. The app
  composition root resolves and injects one implementation of each into memory during startup;
  memory does not call the general routing module to choose them per request. This keeps the core
  edge acyclic while still allowing build profiles to select implementations. At least one
  reference implementation of each is part of every core build closure, even when accelerated or
  remote alternatives are omitted.
- Resolution is startup-only. The composition root reads the build/profile choice, asks each
  memory-owned registry for the named capability, validates model identity/dimension or scoring
  contract plus health, and injects immutable handles. Core memory does not perform per-request
  provider selection, inspect optional-provider concrete types, query the general route catalog,
  or silently fail over between embedders/rerankers. A deployment that wants failover restarts or
  reconfigures through the composition boundary after health validation; a future hot-failover
  design requires a separate approved contract that preserves this dependency direction.
- The first production core profile uses the existing `aimee-llm` HTTP contracts with
  Qwen3-Embedding-4B and Ettin (`ettin-reranker-400m` GPU or `ettin-reranker-68m` CPU) as the
  reference deployment. Their memory-owned HTTP adapters are in the core build closure; the model
  service is an external runtime dependency whose absence makes readiness fail. Deterministic
  fixture providers exist only for contract/unit tests and do not satisfy production readiness.
- **Synthesis is optional.** It can summarize, compose, extract, or generate over recalled memory,
  but disabling it must leave storage, indexing, candidate retrieval, reranking, and evidence
  return fully functional. The synthesis module depends on core contracts; core memory never calls
  its concrete symbols.

Language parsers, OCR, a particular database backend, and additional inference backends may be
optional producers/providers. The knowledge they produce and the APIs used to store, relate,
recall, and reason over it are core memory. Disabling a parser reduces code-memory coverage; it
does not create a separate non-memory ontology.

### Routing

Routing owns typed selection and dispatch policy. The app supplies memory-derived context as typed
IR/routing input; routing does not import memory implementation headers or call memory retrieval.
It also does not import delegate, workflow, roundtable, or provider implementation headers.
Optional modules register routable capabilities. Removing one shrinks the catalog without changing
routing core. Embedding/reranking implementation choice is not general request routing; it remains
inside the memory provider registry described above.

### IR messaging

IR owns canonical requests, responses, tool calls/results, events, attachments, errors, and stream
deltas plus their allocation and ownership rules. Optional stages operate on IR contracts. No
optional module owns a second message representation that the core must understand.

### Translation

Translation core owns the adapter ABI, registry, validation, and IR boundary rules. Concrete wire
and provider adapters may be separately selectable, but every adapter translates through the same
IR. Same-protocol raw bypasses are retired by the related IR-sole-path proposal.

## Dependency law

The allowed direction is:

```text
platform implementations ----> base contracts
                                  |
                                  v
                    memory   routing   ir   translation
                       \        |       |       /
                        +-------+-------+------+
                                        ^
                                        |
                    optional built-in modules / plugins
                                        ^
                                        |
                              app composition roots
```

More precisely:

1. `base` has no feature dependency and stays deliberately small. Moving something to `base`
   requires at least two core consumers and a dependency check proving it imports no module.
2. Core modules may depend on `base` and on explicitly documented core contracts. Cycles between
   core modules are forbidden; shared types move downward or communication crosses an interface.
3. Optional modules may depend on core contracts and on declared optional-module dependencies.
4. Core and base code may not include optional-module headers, link optional-module objects, call
   optional symbols, or branch on an optional module's concrete type.
5. Apps select modules and wire registries. They contain no business logic beyond startup,
   shutdown, dispatch, and transport lifecycle.
6. Cross-module calls use public headers. Including another module's private/internal header is a
   build failure.

There are no core-to-optional compile/link exceptions. The following necessary interactions are
not exceptions: `app/` may include core and selected optional public headers to instantiate them;
core typed registries may validate opaque registrations through core-owned ABI types; and a
default-enabled optional module may contribute capabilities without participating in core health.
None permits core/base to import an optional header, symbol, generated type, or concrete enum.
Plugin validation likewise operates on core-owned registration descriptors; plugin implementation
types remain opaque.

Public module headers use `modules/<name>/include/aimee/<name>/...`; private headers remain below
the module outside `include/`. The dependency checker maps each preprocessor include, exported
header type reference, undefined/link symbol, and selected object back to its owning descriptor.
It emits file:line/symbol evidence for undeclared edges and runs strongly-connected-component
checks on both the module graph and the public-header/symbol graph. Thus a cycle hidden behind a
transitive include or shared concrete struct is not cleared merely because direct includes look
acyclic.

The intended v1 boundary has no direct `memory -> routing` or `routing -> memory` implementation
edge. The app composition root obtains memory context and passes the narrow typed input to routing.
Slice 0B records the actual legacy edge graph and proves the migration to that acyclic boundary.
Where both need a concept, the typed concept belongs in IR or a reviewed narrow base contract;
orchestration belongs in the app composition root.

## Module descriptor and build composition

Each `src/modules/<name>/` gains one machine-readable descriptor. The descriptor set is the single
source of truth. One Slice 0B generator validates it and emits checked-in, deterministically sorted
Make and CMake fragments; neither build system independently discovers module sources. Descriptor
globs are expanded by the generator relative to the descriptor directory, sorted bytewise, and
written as exact paths. Generated-source producers and link-order groups are explicit descriptor
fields. CI regenerates both fragments and fails on any diff, then compares the selected source and
link sets for every profile.

The v1 descriptor contains at least:

```json
{
  "name": "roundtable",
  "class": "optional",
  "default_enabled": true,
  "build_selectable": true,
  "runtime_toggle": false,
  "unavailable_reason": "legacy startup wiring; removed in Slice 6.roundtable",
  "depends_on": ["ir", "routing", "delegates"],
  "required_components": [],
  "registers": ["ir_stage", "command", "service_handler"],
  "sources": ["*.c"],
  "generated_sources": [],
  "link_groups": [],
  "public_include": "include",
  "documentation": "README.md",
  "liveness_manifest": "liveness.yaml"
}
```

Core descriptors use `"class": "core"`, set `build_selectable` and `runtime_toggle` false, and
cannot be disabled. Memory declares `required_components: ["embedding", "reranking"]`; profile
resolution fails if either component has no selected implementation. `registers` names only typed
registries from the approved registry vocabulary, not arbitrary initialization callbacks.
`default_enabled` controls default profile selection only; it does not imply runtime toggleability.
The generated catalog reports `absent`, `present-disabled`, `present-enabled`, or
`dependency-missing` from the selected build plus validated runtime config—those are derived states,
not hand-edited descriptor claims. A generated build fragment replaces
the hand-maintained duplication of source ownership across `CORE_SRCS`, `DATA_SRCS`, `AGENT_SRCS`,
`CMD_SRCS`, KB object lists, and test recipes. Target profiles select modules; descriptors expand
the transitive closure and reject missing or cyclic dependencies.

Runtime configuration is layered on top:

- absent at build time: capability is unavailable and contributes no link dependency;
- present but disabled: registration and lifecycle do not run;
- present and enabled: the module registers tools, routes, stages, providers, or handlers through
  typed core extension surfaces;
- dependency disabled or absent: startup reports a precise configuration error rather than
  silently running a partial module.

The existing plugin context and the IR stage registry are reused and narrowed where possible. A
single universal registration function is not required: memory providers, translators, routing
capabilities, stages, commands, and service handlers should remain typed registries.

## Module documentation contract

Every module is documented individually. A directory name, generated header list, or top-level
architecture paragraph is not sufficient. Each `src/modules/<name>/` owns a `README.md` beside its
descriptor as the authoritative implementation-facing guide, and the documentation build exposes
those guides through a generated module catalog under `docs/modules/`.

Each module document contains, where applicable:

1. **Purpose and boundaries** — what the module owns, what it explicitly does not own, and whether
   it is core or optional.
2. **Public contract** — public headers, stable types and operations, ownership/lifetime rules,
   thread/process assumptions, and compatibility promises.
3. **Dependencies and consumers** — declared module dependencies, why each is necessary, known
   consumers, and the allowed dependency direction.
4. **Lifecycle** — registration, initialization, steady-state workers/hooks/stages, shutdown, and
   unload/disable behavior.
5. **Configuration** — build-profile controls, runtime keys, defaults, required secrets or external
   services, validation, and examples for enabled and disabled states.
6. **Surfaces** — commands, HTTP/RPC/MCP/ACP routes, IR stages, tools, events, plugin extension
   points, and generated descriptors owned by the module.
7. **Data ownership** — schemas/tables, files, caches, indexes, migrations, provenance, retention,
   and which other modules may read or write them.
8. **Failure and degradation semantics** — startup failures, retry/fail-open/fail-closed behavior,
   capability reporting, and operator-visible diagnostics.
9. **Security and trust boundary** — inputs, privileges, credentials, tenant/scope enforcement,
   untrusted data handling, and sensitive outputs.
10. **Observability** — logs, metrics, traces, health/readiness signals, and how to establish that
    the module is actually being exercised.
11. **Testing and verification** — unit, contract, integration, profile-disable, migration, and
    end-to-end tests plus the smallest useful verification commands.
12. **Liveness evidence and removal cost** — the supported journey or non-self consumers that
   justify retaining the module, along with compatibility/data steps required to remove it.

The document ends with a machine-readable `module_evidence` block containing descriptor name/class,
declared dependencies, config keys, commands/routes/registrations, owned data, supported journey
IDs, non-self consumer module+operation pairs, and removal/deprecation references. Empty strings,
free-form claims without IDs, and self-consumers fail validation.

Mechanical coverage is explicit: headings/presence and the evidence block are schema-checked;
dependencies are compared to include/symbol graphs; config keys to the generated config-read
inventory; surfaces/registrations to generated route/command/registry inventories; and owned data
to schema/file inventories. Purpose/boundaries, rationale, lifecycle prose, failure semantics,
security analysis, observability meaning, examples, and removal cost receive a named human review
because correctness cannot be inferred from headings. `docs/modules/_template.md` is the worked
reference and includes acceptable core and optional examples. CI fails distinctly on missing
descriptor, document, heading, structured evidence, inventory, dependency/config/surface/data
drift, unknown journey ID, self-only liveness evidence, or missing external review attestation.

Core-module documents additionally explain why the capability is irreducible and demonstrate the
core round trip it participates in. Optional-module documents include a complete disable/remove
story and identify the behavior and surfaces absent when disabled.

The module descriptor contains a `documentation` path. CI verifies that every descriptor resolves
to a document with the required headings, that declared dependencies/config/surfaces agree with
generated inventories, and that every module appears in the catalog. Generated inventories reduce
drift but do not replace authored rationale, boundaries, failure semantics, or examples.

Documentation changes land in the same slice as code, contract, configuration, route, schema, or
dependency changes. A module extraction is incomplete until its document describes the resulting
module rather than the pre-migration layout.

## Feature-liveness audit: find code that is alive only to itself

Ordinary dead-code analysis is insufficient for this cleanup. A feature can register its own
command or route, call its own helpers, own a schema/table, and have an extensive test suite while
remaining disconnected from every real Aimee workflow. That feature is mechanically reachable but
product-dead. Moving it into a neat module would preserve cost without preserving value.

Before assigning a permanent module owner, build a feature-level reachability and utility map.
Static analysis starts from **non-test shipped roots**:

- binary entry points and their default startup composition;
- public CLI commands that are actually present in generated help/dispatch;
- HTTP/RPC/MCP/ACP routes present in the shipped route descriptor;
- default-on lifecycle hooks, cron/drain workers, and IR stages;
- module/plugin registrations reachable from a shipped/default profile;
- persisted-data readers whose writers are in a different live feature;
- documented end-to-end workflows exercised by integration/eval/dogfood runs.

Tests, benchmarks, fixtures, a feature's own registration call, its own dashboard card, and its own
documentation do **not** independently prove that the feature is useful. They confirm internal
coherence only. A route existing proves exposure, not consumption. A table with one writer and one
reader inside the same isolated subsystem may be an island, not integration.

For each feature cluster, record:

| Evidence | Question |
|---|---|
| Static incoming edges | Which non-test, non-self feature or composition root invokes it? |
| Runtime trace | Which default, integration, dogfood, or explicitly configured workflow executes it? |
| Data flow | Who outside the feature consumes its outputs, rows, events, files, or registrations? |
| User surface | Is the command/route/config documented as part of a coherent supported journey? |
| Duplication | Does another live path already provide the same result? |
| Cost | What production LOC, dependencies, schema, configuration, startup work, and test burden does it add? |
| Core fit | Is it part of memory/routing/IR/translation, an optional capability, or neither? |

Classify every cluster:

1. **core-live** — required by a core round trip and owned by one of the four core modules;
2. **optional-live** — demonstrably used through a supported workflow, but removable from core;
3. **exposed-unproven** — reachable as a command/route/flag but with no external consumer or
   end-to-end workflow evidence;
4. **self-contained island** — implementation, tests, data, and registration refer primarily to
   one another and no meaningful non-self consumer exists;
5. **duplicate/legacy** — a second path, fallback, superseded experiment, or compatibility layer
   whose replacement is live;
6. **unknown** — insufficient evidence; assigned an owner and a time-bounded investigation rather
   than silently retained.

The audit produces both a human-readable report and a machine-readable manifest containing the
feature id, source/header/test/schema/config/route sets, incoming and outgoing feature edges,
profiles, runtime evidence, classification, decision, and rationale. The graph must collapse
file/symbol edges into feature clusters and exclude test-only edges by default, with an explicit
view that shows how much apparent reachability comes only from tests.

Disposition follows evidence:

- `core-live`: simplify and migrate into its core owner;
- `optional-live`: simplify, migrate, and add a real disable/absence test;
- `exposed-unproven`: instrument locally, exercise deliberately, then either document a supported
  workflow or deprecate/delete it;
- `self-contained island`: delete implementation, tests, config, schema, routes, docs, and build
  entries together unless a concrete consumer is identified;
- `duplicate/legacy`: migrate remaining callers to the authoritative path and delete it behind the
  applicable parity/deprecation gate;
- `unknown`: do not modularize yet; resolve the evidence question first.

A public surface may require a deprecation window even when internally unused. That is a
compatibility decision, not proof the implementation belongs in the permanent architecture. A
thin compatibility adapter may temporarily call the surviving core/module path; the duplicate
implementation does not survive with it.

Aimee does not add phone-home telemetry for this audit. Runtime evidence comes from local opt-in
traces, CI integration journeys, benchmarks/evals, and project dogfood. Lack of observed execution
is not alone proof of death, but lack of execution **plus** no non-self consumer, no coherent
supported journey, and a duplicate or isolated data flow is strong deletion evidence.

### Cluster definition, evidence, and deadlines

The initial cluster map is not inferred circularly from the graph it is meant to judge. It starts
from existing `src/modules/<name>/` ownership plus the complete Initial ownership map below. Every
production file outside those sets is assigned explicitly or reported as `unassigned`, which fails
Slice 0A. Each cluster manifest records its files, public symbols, registration/init files, test
files, generated artifacts, schema/config/surface sets, and proposed owner.

A non-self consumer is a non-test call/data/include edge from cluster A to a public operation or
output of cluster B where A differs from B and the edge is not merely B's own registration/init
path. Registration files are an explicit per-cluster set, not guessed by filename. A supported
journey is a versioned manifest entry with an entry command/route, ordered observable effects,
expected data/output, and an integration/eval/dogfood test ID that exercises it. A dashboard card,
help entry, unit test, or route alone cannot be that journey.

Evidence sources are named and reproducible: the default personal-memory, shared-KB, code-memory,
provider, delegate, workflow, and web integration journeys; the memory/retrieval and code-graph
evals; and local opt-in dogfood traces stored as redacted structural event manifests. Dogfood alone
cannot promote a cluster to `optional-live`; it needs a non-self static/data consumer or a checked
supported journey. Classification or override approval requires a reviewer outside the cluster's
owner, with evidence links in the manifest. Challenges and later-reported missed consumers reopen
the disposition; any reintroduction PR includes an audit-gap postmortem and updates the detection
rule or journey inventory that missed it.

`unknown` has a 30-day resolution deadline; `exposed-unproven` has a 90-day deadline. On expiry CI
fails until the cluster is promoted with evidence or receives an approved deletion/deprecation
record. Neither classification may be migrated into a permanent module. `core fit: neither`
defaults to `self-contained island` unless supported-journey evidence promotes it to
`optional-live`. The machine manifest stores `owner`, `classified_at`, `disposition_deadline`,
`reviewer`, and any signed override; overrides cannot be approved by the owning module alone.

## Initial ownership map

This table is a migration guide, not permission for blind filename moves. Each slice verifies
callers, headers, target links, and tests before changing paths.

| Current family | Target owner |
|---|---|
| `index.c`, `extractors*.c`, `code_*`, `rel_types.c`, graph and blast-radius logic | `modules/memory/code/` |
| `memory_*`, session/episode/history/working-memory implementations | `modules/memory/` modality subdirectories |
| embedding/reranking probes, clients, metadata, execution, dimension/score guards | `modules/memory/inference/{embedding,reranking}/` |
| generative answer composition, summarization, and extraction over recalled evidence | optional `modules/synthesis/` (stored evidence/provenance remains memory-owned) |
| curation, deduplication, contradiction handling, promotion/demotion, calibration, and learning from outcomes | `modules/memory/` lifecycle/learning owners unless the audited implementation only generates presentation text |
| memory-bearing portions of `db1/`, `db2/`, and `kb/` | `modules/memory/{personal,shared,storage,service}/` |
| `aimee_ir_*`, IR metrics/shadow/stream structures | `modules/ir/` |
| `aimee_backend_*`, `openai_*`, `anthropic_*`, Responses, Bedrock mapping, provider/CLI wire conversion | `modules/translation/` |
| auxiliary/model/provider/capability route selection and route catalogs | `modules/routing/` |
| `plugin*.c` and plugin-owned headers | `modules/extensions/` |
| feature-specific `cmd_*.c` and `cli_*.c` | the owning module's `cli/` adapter |
| feature-specific HTTP/RPC handlers in `server_state`, `server_http_routes`, or KB service dispatch | the owning module's `service/` adapter |
| delegate, workflow, roundtable, git, guardrail, audit, vault, CSS, skill, roadmap, LSP, sandbox logic | their existing `modules/<name>/` owner |
| listener loops, process lifecycle, signal handling, generic dispatch | `app/<binary>/` |
| OS-specific sockets, process, keychain, and TLS implementations | `platform/<os>/` behind base/core contracts |
| generated route, schema, CLI-help, and tool-prompt data | `generated/` |

Provider/model selection belongs to routing; conversion to and from a provider's wire format
belongs to translation; HTTP execution belongs to a transport adapter. A file doing all three must
be split at those seams rather than assigned by its filename.

## Slices

Each slice lands as an independently buildable PR. Public behavior remains compatible unless the
slice carries an approved deletion/deprecation decision, but internal behavior, types, and APIs may
change substantially. A successful slice should normally remove more production complexity than it
adds; broad path-only moves use `git mv`, while mixed-responsibility files are deliberately split
and consolidated instead of preserved for history's sake.

### Slice 0A — feature liveness, utility, and deletion audit

- Build the feature-cluster graph described above from non-test shipped roots, with test-only and
  self-registration edges labeled separately.
- Trace the default personal, shared-KB, provider, code-memory, delegate, workflow, and web journeys
  and map observed execution back to feature clusters.
- Inventory commands, routes, flags, config fields, schemas/tables, workers, stages, plugins,
  dashboards, and generated descriptors with their non-self consumers.
- Publish the human and machine-readable liveness reports and classify every feature cluster.
- Produce proposed retain, deprecate, consolidate, or delete dispositions, including the complete
  implementation/test/config/schema/route/documentation set affected by each deletion. Do not
  delete in this slice: the compatibility and rollback baseline in Slice 0B must exist first.
- Store one independently reviewable disposition record per cluster. It contains the external
  reviewer/approval, evidence-package reference, complete touch/removal set, public-compatibility
  decision, deprecation deadline/removal condition where applicable, and named rollback owner. An
  aggregate audit approval cannot authorize hundreds of independent removals.
- Create a time-bounded investigation item for every `unknown`; unknown code is not waved through
  as permanently live merely because the audit is hard.

**Gate:** every production source family has a feature classification and independently approved
disposition record; retained optional features have at least one non-self consumer or supported
end-to-end journey; no `unknown`/`exposed-unproven` record lacks its owner/deadline; and every
proposed deletion names its public-compatibility decision, complete removal set, and rollback owner.

### Slice 0B — freeze the dependency graph and add enforcement

- Generate current file-to-target, include, exported-header-type, undefined-symbol/link, generated
  artifact, and feature-cluster edge inventories for root, server, KB, DB1, DB2, headers, and
  existing modules. Publish every current core↔core and core→optional violation with file:line or
  symbol evidence and a proposed resolution: move the call site, narrow an IR/base contract,
  orchestrate in `app/`, or obtain an explicit architecture decision. Test enforcement against
  this reviewed inventory before descriptor-selected builds become authoritative.
- Add the module descriptor schema and generate Make/CMake source fragments from it.
- Define the profile interfaces in this slice: Make uses
  `make -C src module-profile PROFILE=<name>` and, separately,
  `make -C src module-profile-test PROFILE=<name>`; CMake uses
  `cmake -S . -B build/<name> -DAIMEE_PROFILE=<name>` followed by target
  `aimee-<name>-profile` or `aimee-<name>-test`. Both consume the same generated descriptor
  fragments and exact target shapes specified below; Slice 0B creates these targets before any
  later acceptance command references them.
- Add checks for undeclared cross-module includes, private-header imports, dependency cycles, and
  core-to-optional edges.
- Add the module-document schema/catalog generator and documentation completeness/drift checks.
- Add a temporary explicit allowlist for files not yet moved. The allowlist may only shrink.
- Create `scripts/compare_surface_baseline.sh` and record its versioned inputs under
  `tests/baselines/modules/`: every shipped CLI command name/argument/help shape; generated HTTP,
  RPC, MCP and ACP route descriptors; representative success/error/stream IR fixtures; config
  schema/default keys; DB1/DB2 schema+migration digests; public installed-header and symbol lists;
  plugin manifest/ABI fixtures; and package/install manifests. Exact stable fields are byte-equal;
  explicitly normalized volatile fields are semantic-equal; every other diff fails unless linked
  to a separately approved compatibility record.
- Generate authoritative module surface, command, route, schema, config-read, and registration
  inventories used by documentation drift checks. Slice 0B owns the descriptor generator,
  dependency checker, surface-baseline script, documentation catalog/checker, and their fixtures.
- Record the cleanup baseline: production LOC/files, internal types, registries, source lists,
  compatibility flags, pass-through wrappers, and duplicate implementations by feature family.

**Gate:** default Make and CMake builds consume byte-for-byte regenerated descriptor fragments and
equivalent source/link selections; module and public-header/symbol graphs report no unrecorded
edges; the dependency check describes every temporary legacy exception; all named baseline
families and authoritative inventories exist; and every later slice has a reproducible baseline
against which compatibility and simplification can be measured.

### Slice 0C — execute approved deletion and consolidation decisions

- Apply the Slice 0A dispositions only after the Slice 0B surface, schema, build, and cleanup
  baselines are reproducible.
- Cross-check every proposed deletion against the four core descriptors' `required_components`
  and a provisional core-round-trip stage inventory; an intersection blocks deletion pending an
  architecture decision.
- Delete clear self-contained islands as complete units: implementation, self-tests, config,
  schema/migrations where safely removable, routes/commands, documentation, generated entries,
  and build ownership.
- For duplicate/legacy paths, prove the surviving implementation satisfies the recorded public
  fixtures, migrate remaining non-self callers, and delete the duplicate rather than wrapping it
  behind the new module boundary.
- For public surfaces requiring a deprecation window, retain only a thin compatibility adapter,
  record its removal condition and deadline, and delete the duplicate implementation immediately.
- Re-run the liveness graph after deletion so removal cannot strand a newly isolated feature.
- Before each deletion, attach its complete touch set: source/header/test and test-data files,
  config keys, schema/tables/migrations, generated entries, routes/commands, manifests, and all
  incoming edges. After deletion, prove the touch set is absent from non-deleted artifacts (except
  an explicitly time-bounded compatibility adapter), the liveness graph has no remaining non-test
  incoming edge, and the surface baseline is unchanged or linked to its approval record.
- Database migrations are append-only after deployment: Slice 0C does not delete or reverse an
  applied migration. First stop new writes and remove readers behind a compatibility window; a
  later data-retirement migration may remove dormant columns/tables after backup/export evidence.
  Once schema state advances, rollback is a forward fix or prior-image rollback against a retained
  compatible schema, never a blind git revert. Before Slice 0C, retain a tested pre-deletion image
  and release tag/branch through the deletion window. Public-surface and persisted-data removals
  require separately approved compatibility records with owner, effective date, window, and
  recovery procedure.

**Gate:** every executed disposition matches its independently approved record and per-deletion
touch-set proof; surface/schema fixtures remain equal or carry a separately approved compatibility
change; the retained image/tag and forward-fix/data recovery procedure are tested where state can
advance; and the production cleanup ledger demonstrates a net reduction in the affected family.

### Slice 1 — establish the four mandatory core modules

- Finish co-locating memory public/private headers with `modules/memory/`.
- Establish embedding and reranking as mandatory memory submodules, including their typed provider
  contracts, model/dimension/score metadata, startup resolution, and working reference
  implementations.
- Extract synthesis behind an optional-module contract; core memory returns ranked evidence
  without requiring synthesis to be present.
- Move IR types, ownership, metrics, shadow, and delta code from server/global headers into
  `modules/ir/`.
- Extract routing contracts, catalog, and policy from server/delegate/provider-specific files into
  `modules/routing/`.
- Extract translation registry/contracts and existing provider backend mappers into
  `modules/translation/`.
- Make each core module independently testable and mark all four `class: core`.
- Write the individual memory, routing, IR, and translation module documents, including their
  irreducibility arguments and shared core round trip.
- Resolve cycles via interfaces; do not create a catch-all `core.h`.
- Consolidate duplicate core types/helpers and delete superseded internal representations as their
  callers move; do not reproduce old library buckets inside the new directories.

**Gate:** the exact Make targets `aimee-core-objects`, `aimee-core`, `aimee-core-profile`, and
`aimee-core-test` and CMake targets `aimee_mod_memory`, `aimee_mod_routing`, `aimee_mod_ir`,
`aimee_mod_translation`, `aimee_core_core`, `aimee-core-profile`, and `aimee-core-test` build with
no optional-module object or header dependency,
and `tests/core_round_trip/core_round_trip.json` passes through the canonical
`scripts/test_core_round_trip.sh`. The test builds only the four core descriptors plus
`app/core-smoke`, injects the non-production fixture providers specified below, asserts with the
link map/symbol ownership manifest that no optional
object/header/symbol
is present, starts with the SQLite reference storage adapter, and reports provider contract
identities. A separate production-provider readiness test exercises the compiled `aimee-llm`
embedding/reranking adapters in the external-service CI tier; production startup cannot substitute
the fixture providers. The deterministic fixture performs
write → embed → candidate retrieval → rerank → route → IR response → loopback JSON translation,
asserts relevant evidence at the public boundary, and asserts synthesis is absent from both the
link closure and capability catalog. A synthesis request returns typed `unavailable`, never a
silent pass-through or null failure. This gate lands in Slice 1 and stays green thereafter.

### Slice 2 — put code intelligence wholly inside memory

- Move `index.c`, `extractors*.c`, `code_collect.c`, `code_match.c`, `code_outline.c`,
  `code_treesitter.c`, `code_audit_graph.c`, `rel_types.c`, and their owned headers into
  `modules/memory/code/`.
- Move KB-side code graph, code vector, cross-repo graph, architecture, and blast-radius
  implementations into the same memory-owned subtree, separated into storage/service adapters as
  needed.
- Keep language-specific extractors optional and capability-reported. Keep the code-memory schema,
  ontology, query contracts, graph relations, and recall surface core.
- Rename user-facing documentation only where it incorrectly presents code intelligence as a
  peer subsystem instead of a memory modality; command compatibility remains unchanged.

**Gate:** index/search/find-symbol/graph/blast-radius tests pass through memory-owned public APIs,
and no code-intelligence implementation remains at `src/` root.

### Slice 3 — move personal and shared memory service/storage code

- Re-home DB1 personal/session-memory implementations under `modules/memory/personal/` and their
  storage adapter.
- Re-home DB2 shared/corpus/code-memory implementations under `modules/memory/shared/` and their
  storage adapter.
- Re-home KB memory algorithms—curation, deduplication, contradiction handling, ranking,
  calibration, reflection, retrieval, and lifecycle—under memory-owned submodules.
- Move embedding and reranking integrations currently split across root, KB, DB, platform, probe,
  and client families into the mandatory memory inference owners. Additional local, accelerated,
  or remote providers remain replaceable; the operations do not become optional.
- Move only generative answer composition, summarization, and extraction over recalled evidence
  into the optional synthesis module. Curation, deduplication, contradiction handling,
  promotion/demotion, calibration, outcome learning, and stored facts/evidence/provenance remain
  memory-owned unless Slice 0A proves a specific implementation is a removable island.
- Collapse duplicate DB1/DB2 query construction, row mapping, scope handling, and result shaping
  behind the smallest storage contracts that their real differences permit.
- Leave `app/kb/` with process startup, connection composition, listener lifecycle, and generic
  dispatch only.
- Preserve DB1/DB2 schema compatibility and deployment migration paths.

**Gate:** personal and shared deployment profiles pass existing DB, migration, ingest, recall, and
code-graph suites with unchanged wire fixtures. Versioned migration fixtures prove the new binary
opens and preserves databases created by the pre-refactor release; the retained prior binary reads
all still-supported data written before any separately approved schema expansion. Applied
migrations remain append-only, and downgrade-incompatible writes require an explicit compatibility
decision rather than being hidden inside a move.

### Slice 4 — extract translation and routing from the server host

- Move provider profiles, model catalogs, auxiliary routing, failover policy, and capability
  selection into `routing` or a declared optional provider module as appropriate.
- Move OpenAI, Anthropic, Responses, Bedrock, CLI-agent, MCP/ACP, SSE, and other message conversion
  into translation adapters operating only through IR.
- Keep sockets/listeners/retry transport in narrow transport owners and application composition.
- Coordinate with `ir-sole-path-and-pluggable-stages.md`; do not preserve a legacy raw path merely
  to ease a move.

**Gate:** default routes and provider fixtures are byte/semantic-parity clean under the IR
proposal's evidence gates, and server composition imports only public routing/IR/translation APIs.

### Slice 5 — move command and service adapters to their feature owners

- Move each feature-specific root `cmd_*.c`/`cli_*.c` into that feature module's `cli/` adapter.
- Replace global command-table knowledge with typed command registration generated or assembled by
  the selected module set.
- Move feature-specific server and KB route handlers into module `service/` adapters; leave a
  generic dispatcher in each app.
- Move dashboard and web-only behavior into optional dashboard/web modules.

**Gate:** generated CLI help and route descriptors are unchanged for the default profile; disabling
an optional module removes its commands/routes cleanly and returns the documented unavailable
response rather than leaving a dangling handler.

### Slice 6 — finish existing optional-module extractions

- Sweep root/server/KB families for delegates, workflows, roundtables, governance, guardrails,
  audit, git/forge, workspace, sandbox, skill, roadmap, eval, CSS, LSP, economizer, manuscript,
  persona, web-user, and other non-core capabilities.
- Move implementation and private headers into the existing or newly declared owner.
- Split mixed files at core/optional boundaries rather than moving a core dependency into an
  optional module.
- Add build and runtime enable/disable tests for each module as its seam becomes safe.
- Write or refresh each optional module's individual documentation, including its supported
  journey, non-self consumers, failure semantics, and complete disable/remove story.
- Execute this as independently reviewed sub-slices by dependency closure, not one omnibus PR:
  `6.delegation` (delegates/workflows/roundtable), `6.governance`
  (governance/guardrails/audit), `6.workspace` (git/forge/workspace/sandbox), and one sub-slice per
  remaining skill/roadmap/eval/CSS/LSP/economizer/manuscript/persona/web-user owner.

**Gate:** every optional module has a descriptor, declared dependencies, an ownership test, and at
least one disabled-profile test; its module documentation passes completeness and drift checks.

### Slice 7 — collapse global headers and historical build buckets

- Move owned headers out of `src/headers/`; retain only temporarily forwarded compatibility headers
  where an external public include path requires a deprecation window.
- Retire broad historical source buckets in favor of descriptor-selected modules and small app
  targets.
- Move generated files to `src/generated/`, platform files to `src/platform/`, and dependency-free
  shared primitives to the reviewed `src/base/` allowlist.
- Delete forwarding headers after all in-tree consumers migrate.
- Delete obsolete source buckets, compatibility flags, unused registries, duplicate generators,
  and pass-through wrappers identified by the cleanup and liveness ledgers.
- Generate the complete `docs/modules/` catalog from module descriptors and validate all authored
  module documents against the final ownership/dependency/surface graph.
- Create a machine-readable root allowlist and `scripts/check_src_root_allowlist.sh`. It permits
  only the named build/readme/manifest entries and fails on every other root entry. A separate
  forwarding-header check rejects `src/headers/` compatibility headers once their recorded
  external window closes or once a migrated module still imports one. Public installed headers
  retain their supported include paths through at least one declared compatibility release; only
  private/global-bucket paths disappear immediately.

**Gate:** no module includes another module's private header; Make and CMake agree on the selected
module graph; the root allowlist and forwarding-header checks pass; plugin manifest format,
registration signatures, discovery paths, and public installed-header fixtures remain compatible;
install/package manifests contain every public header and selected module artifact.

### Slice 8 — prove minimal and configurable products

Build and test at least these profiles:

1. **core:** memory + routing + IR + translation, with working embedding and reranking plus the
   SQLite reference storage adapter, the memory-owned `aimee-llm` HTTP embedding/reranking
   adapters, one fixed routing capability, the core request/evidence IR types, and the loopback
   JSON translator used by `app/core-smoke`; synthesis and every optional descriptor are absent;
2. **personal:** core + personal memory + chosen agent/provider adapters;
3. **shared-kb:** core + shared storage, ingest, curation, and code-memory producers;
4. **full:** today's default bundled product;
5. **full-minus-one matrix:** generated from the descriptor catalog and run with every optional
   module disabled in turn. Required/core descriptors are excluded by schema, not by a hand list;
   all rows run in CI, with slow external-service rows eligible for a required nightly tier only
   when the PR tier still performs their build/link/absence checks.

The core profile is not required to provide every wire/provider/database implementation. It must
provide every core contract and at least one reference adapter per boundary so the substrate is
executable and testable. Embedding and reranking must resolve to working implementations and pass a
write -> candidate retrieval -> rerank round trip. Typed absence is valid for synthesis and other
optional capabilities, not for embedding or reranking.

**Gate:** every profile builds and starts; core behavior is identical across profiles; optional
module absence never changes the core ABI or creates unresolved symbols. Slice 8 creates
`scripts/test_module_profiles.sh` and the canonical core-round-trip harness. The core fixture also
proves embeddings are distinguishable for semantically distinct inputs and the reranker
non-trivially reorders a known embedding-only misranking; a constant embedder or pass-through
reranker fails.

## Normative implementation contracts

The artifacts and rules in this section are part of the proposal, not illustrative follow-up work.
Slice 0 must land them before descriptor-selected builds or any deletion becomes authoritative.

### Descriptor, evidence, journey, and disposition schemas

The checked-in schemas are `src/modules/module.schema.json`,
`docs/modules/module-evidence.schema.json`, `docs/journeys/journey.schema.json`,
`docs/audit/feature-cluster.schema.json`, `docs/audit/disposition.schema.json`, and
`docs/audit/compatibility-record.schema.json`. They use JSON Schema 2020-12; YAML documents are
validated after YAML-to-JSON decoding. No additional properties are allowed in v1.

`module.schema.json` requires the following fields and types:

| field | type and constraint |
|---|---|
| `schema_version` | constant `1` |
| `name` | string matching `^[a-z][a-z0-9-]*$` |
| `class` | enum `core`, `optional` |
| `default_enabled` | boolean; must be `true` for core |
| `build_selectable` | boolean; must be `false` for core |
| `runtime_toggle` | boolean; must be `false` for core |
| `unavailable_reason` | null or `slice:<id>: <reason>`; required and non-null only while an optional module is not build-selectable |
| `depends_on` | unique array of module names |
| `required_components` | unique array from `memory.storage`, `memory.embedding`, `memory.reranking`, `routing.selector`, `ir.pipeline`, `translation.adapter` |
| `registers` | unique array of objects `{kind,id}`; `kind` is one of `memory_storage_provider`, `memory_embedding_provider`, `memory_reranking_provider`, `routing_capability`, `ir_stage`, `translator`, `command`, `service_handler`, `plugin_manifest`, `tool`, or `event` and `id` is a namespaced stable identifier |
| `sources` | unique array of repository-relative regular source paths; no globs |
| `generated_sources` | unique array of `{path,producer}`; the producer is a declared generator target and the path must exist after it runs |
| `link_groups` | array of `{name,members,after}`; names and members are unique and `after` forms a DAG |
| `public_include` | unique array of paths matching `src/modules/<name>/include/aimee/<name>/**` |
| `documentation` | path equal to `docs/modules/<name>.md` |
| `liveness_manifest` | path to a cluster manifest conforming to `feature-cluster.schema.json` |

The validator rejects an authored availability-state field. `scripts/derive_module_state.py` derives
`absent`, `present-disabled`, `present-enabled`, or `dependency-missing` from the validated
descriptor catalog, selected profile, runtime config, and resolved required-component registry.
Every required component must have exactly one selected implementation. New registry vocabulary is
an architecture-contract change requiring an independent architecture approval and a schema-version
change. Descriptor sources and registrations cannot reintroduce a path or ID marked removed in a
cleanup ledger without a new approved disposition record.

The `module_evidence` fenced YAML block in each module document requires: `schema_version`,
`module`, `class`, `dependencies`, `config_keys`, `surfaces` (typed `{kind,id}` values), `owned_data`
(typed `{kind,id}` values), `journeys`, `consumers` (objects `{module,operation,evidence}`),
`deprecations`, `last_verified_at`, and `human_attestation`. Arrays may be empty only when the
generated inventory for that category is empty. IDs must resolve; prose is not evidence. A consumer
whose module equals the documented module, or whose operation resolves only to that module's own
init, registration, tests, docs, migrations, or dashboards, is self-only and fails for retained
optional modules. Descriptors and generated inventories are authoritative; documentation mismatch
is a failure, never an override. `human_attestation` is `{reviewer,reviewed_commit,sections}`; the
reviewer must be listed in `docs/audit/reviewers.yaml`, differ from the module owner, approve the
exact commit through the protected-branch review record, and attest all eight human-review sections.

`docs/journeys/registry.yaml` contains entries conforming to `journey.schema.json`: required
`id`, `owner`, `entry_surface`, ordered `observable_effects`, `expected_outputs`, and at least one
non-self `test_id` from the generated test inventory. Unknown IDs and tests that only invoke a
cluster's internal symbols fail.

Feature manifests live at `docs/audit/clusters/<cluster>.yaml` and require `id`, `owner`,
`source_set`, `header_set`, `test_set`, `registration_set`, `schema_set`, `config_set`, `route_set`,
typed incoming/outgoing edges with file-and-symbol endpoints, selected profiles, runtime evidence,
classification, `classified_at`, deadline, and disposition path. Classification is one of the six
values in Slice 0A. Multi-module clusters have one accountable owner and list all participant
modules. A disposition at `docs/audit/dispositions/<cluster>.yaml` requires its own `cluster_id`,
`owner`, non-owner `reviewer`, `reviewed_commit`, `attestation: approved`, non-empty evidence links,
decision, complete touch set, compatibility-record references, rollback owner, `classified_at`, and
deadline. One aggregate approval cannot cover multiple cluster IDs.

Slice 0A also checks in six schema-valid, non-production examples under
`docs/audit/examples/{core-required,optional-live,exposed-unproven,self-contained-island,duplicate-legacy,unknown}.yaml`.
They demonstrate the required evidence, independent attestation, touch set, compatibility choice,
and deadline for every classification; the audit test suite validates them as contract fixtures.

The liveness checker treats an edge as self-only when its producer and consumer endpoints are both
in the cluster sets, or its consumer is in that cluster's explicit registration set, migration set,
documentation/dashboard set, or a test path matching `tests/**`, `**/*_test.*`, or `**/test_*.*`.
It validates a non-self edge by resolving both endpoint files and symbols against the ownership
inventory and requiring a different cluster plus a supported operation, or a valid journey ID.
Deadline arithmetic uses the ISO-8601 UTC `classified_at` date: unknown expires at 30 days and
exposed-unproven at 90. Expiry blocks CI; there is no automatic reclassification. The owner must
provide evidence or an independently approved deletion/deprecation record. An extension is a new
disposition with a different reviewer and a maximum single 30-day extension.

### Deterministic build generation and exact targets

`scripts/generate_module_builds.py` reads explicit descriptor paths in UTF-8 bytewise path order;
it never expands globs or uses filesystem order, timestamps, locale, or absolute paths. It writes
checked-in `src/generated/modules.mk`, `cmake/generated/modules.cmake`, and
`tests/baselines/modules/ownership.json`. Generation is content-only and byte reproducible; there
are no normalized volatile fields. `scripts/check_generated_module_builds.sh` generates into a
temporary directory, performs byte comparisons, compares sorted source/object/link-group manifests
between Make and CMake, and exits 1 with a unified diff on any drift (2 for invalid input).

For every profile `<p>`, both build systems expose exactly these target concepts:

- `aimee-<p>-objects`: all selected module objects;
- `aimee-<p>-core`: a static archive containing selected objects in descriptor/link-group order;
- `aimee-<p>-profile`: an executable linked from that archive and the profile app entry point;
- `aimee-<p>-test`: a target that builds the executable and runs that profile's declared tests.

Make implements them in one included fragment, with objects at
`build/make/<p>/modules/<module>/<source-path>.o`, compiler dependency files beside each object, and
`ar rcsD` for deterministic archives. `module-profile` is only a checked alias: it requires
`PROFILE`, depends on `aimee-$(PROFILE)-profile`, and the separate `module-profile-test` alias
depends on `aimee-$(PROFILE)-test`. CMake emits one `OBJECT` library `aimee_mod_<module>`, one
`STATIC` library `aimee_<p>_core`, one `add_executable(aimee-<p>-profile ...)`, and one
`add_custom_target(aimee-<p>-test ...)`; selected object libraries are the only transitive link
input. A link group is emitted as `--start-group/--end-group` in Make and `LINK_GROUP:RESCAN` in
CMake after a topological sort of `after`; a cycle is invalid.

The normative generated core shape is therefore:

```make
.PHONY: aimee-core-objects aimee-core-profile aimee-core-test
aimee-core-objects: $(AIMEE_CORE_OBJECTS)
build/make/core/libaimee_core.a: aimee-core-objects
\t$(AR) rcsD $@ $(AIMEE_CORE_LINK_INPUTS)
aimee-core-profile: build/make/core/aimee-core-profile
aimee-core-test: aimee-core-profile
\tscripts/test_core_round_trip.sh --profile core --binary build/make/core/aimee-core-profile --fixture tests/core_round_trip/core_round_trip.json
```

```cmake
add_library(aimee_mod_memory OBJECT ${AIMEE_MEMORY_SOURCES})
add_library(aimee_core_core STATIC $<TARGET_OBJECTS:aimee_mod_memory> # plus routing, ir, translation
)
add_executable(aimee-core-profile ${AIMEE_CORE_APP_SOURCE})
target_link_libraries(aimee-core-profile PRIVATE aimee_core_core)
add_custom_target(aimee-core-test COMMAND ${CMAKE_SOURCE_DIR}/scripts/test_core_round_trip.sh --profile core --binary $<TARGET_FILE:aimee-core-profile> --fixture ${CMAKE_SOURCE_DIR}/tests/core_round_trip/core_round_trip.json DEPENDS aimee-core-profile)
```

CI runs `make -C src module-profile PROFILE=core`, `make -C src module-profile-test PROFILE=core`,
`cmake -S . -B build/core -DAIMEE_PROFILE=core`, `cmake --build build/core --target
aimee-core-profile`, and `cmake --build build/core --target aimee-core-test`. The full-minus-one PR
tier builds, links, and proves symbol/catalog absence for every optional module; runtime rows without
external services also execute. External-service runtime rows execute in the mandatory nightly tier.

### Core provider ABI and round-trip fixture

Memory owns the public provider ABI at `src/modules/memory/include/aimee/memory/providers.h`.
`aimee_embedding_provider_v1` contains ABI version, provider/model IDs, dimension, `init`, `fini`,
`validate_model`, and `embed_batch`; the caller owns input strings and the provider writes exactly
`count * dimension` finite normalized floats into caller-owned output. `aimee_reranking_provider_v1`
contains ABI version, provider/model IDs, `init`, `fini`, `validate_model`, and `rerank`; it returns
one finite score and stable permutation over caller-owned candidates. Both return the shared enum
`AIMEE_PROVIDER_OK`, `AIMEE_PROVIDER_UNAVAILABLE`, `AIMEE_PROVIDER_MODEL_MISMATCH`,
`AIMEE_PROVIDER_DIMENSION_MISMATCH`, or `AIMEE_PROVIDER_INVALID_OUTPUT`.

Production `core` compiles the `aimee-llm` HTTP adapters into memory and readiness probes
`memory.embedding.ready` and `memory.reranking.ready` validate endpoint, model ID, embedding
dimension, output cardinality, finite values, and rerank permutation. Startup is non-ready with the
specific error above if either probe fails. Deterministic `fixture_embedding_v1` and
`fixture_reranking_v1` are linked only into the `aimee-core-test` harness through test injection;
they have no descriptor, profile, production catalog entry, or installed header.

`tests/core_round_trip/core_round_trip.schema.json` requires `schema_version`, provider metadata,
three or more memories `{id,text}`, query, `embedding_assertions`, `embedding_candidate_order`,
`expected_rerank_order`, route ID, expected IR response, and expected translated JSON.
`core_round_trip.json` uses memories `m1: "C parser resolves call symbols"`,
`m2: "Banana bread uses ripe bananas"`, and `m3: "C call graph records caller and callee"`; query
`"Which memory explains C caller relationships?"`. It requires every distinct pair's L2 distance
to exceed `1e-6`, the best relevant-vs-irrelevant cosine margin to be at least `0.10`, embedding
candidate order `[m1,m2,m3]`, and reranked order `[m3,m1,m2]`; thus `m3` must move from rank 3 to
rank 1, identity reranking fails, and NDCG@3 must be at least `0.95`. A constant embedder fails both
distance predicates.

The harness records and asserts named registry traversal:
`memory.storage/sqlite-reference.write`, `memory.embedding/selected.embed`,
`memory.storage/sqlite-reference.candidates`, `memory.reranking/selected.rerank`,
`routing.capability/core.fixed.select`, `ir.stage/core.response.shape`, and
`translation.adapter/core.loopback-json.encode`. The translator contract is
`src/modules/translation/include/aimee/translation/loopback_json.h`; core registers it directly,
accepts the canonical IR response, and emits the fixture's canonical sorted-key JSON without an
optional provider.

The harness dumps `build/core/capabilities.json` and sorted global undefined/defined symbols. It
requires no catalog kind or ID matching `synthesis` and no linked symbol with prefixes
`aimee_synthesis_`, `aimee_answer_compose_`, or `aimee_generative_summary_`; the closed prefix list
lives in `tests/core_round_trip/forbidden_synthesis_symbols.txt`. Each stage above must appear once
in `build/core/core-round-trip-trace.json` with the expected input/output ID; a partial smoke fails.

### Dependency graph algorithm and cycle policy

Public headers are exactly regular files matching
`^src/modules/([^/]+)/include/aimee/\1/.+\\.h$`; all other module headers are private. A public
header may include another module's public header only when `depends_on` declares that module. It
may not include another module's private header. Cross-core value-type references are allowed, but
the complete transitive public-include/type graph must be acyclic; optional modules may depend on
core, never the reverse. Opaque forward declarations produce no type edge until a field, inline
body, macro body, typedef expansion, or function-by-value signature requires the definition.

`scripts/check_module_deps.sh` obtains include edges from compiler depfiles plus preprocessor line
markers, public type/function/global/macro references from Clang AST/preprocessor output, and
defined/undefined global symbols from `nm -A` over the exact profile object manifest. Ownership is
the longest descriptor source/generated/public-include path match, checked against the generated
object-to-module and symbol-to-module maps. Generated headers are scanned identically; their
producer creates an implicit edge that must be in `depends_on`. Link SCC input is the selected
object list in actual resolved link-group order. Tarjan SCCs run over module edges and the expanded
public-header/type/symbol graph; any multi-node SCC or self-loop caused by a forbidden import fails.

Output is sorted JSON Lines at `build/reports/module-deps.jsonl` plus stderr of the form
`E_CORE_OPTIONAL src/modules/memory/x.c:42 symbol=aimee_chat_send owner=memory target=chat`.
Exit 0 means clean, 1 violations, 2 invalid/unowned input. Slice 0B freezes
`tests/baselines/modules/dependency-ownership.json`; until cutover CI reproduces it byte-for-byte
from both the legacy selection and descriptor selection before using descriptors authoritatively.

### Surface baselines and equality rules

`tests/baselines/modules/index.yaml` is authoritative and names the baseline version, owner, format,
normalizer, and compatibility-record field for every file:

| path | equality and normalization |
|---|---|
| `cli-help.txt` | byte-equal after replacing only version, build date, and workspace prefix tokens |
| `routes.json` | semantic JSON after sorting by protocol/method/path; all route fields byte-equal |
| `ir-success.json`, `ir-error.json`, `ir-stream.jsonl` | semantic JSON; only request IDs, trace IDs, timestamps, and measured latency are tokenized; at least one fixture of each kind per protocol |
| `config-schema.json`, `config-defaults.json` | semantic sorted-key JSON; no fields normalized |
| `db1-schema.sha256`, `db2-schema.sha256` | byte-equal SHA-256 of normalized SQLite `.schema` output with whitespace canonicalized, followed by ordered migration ID and content-digest pairs; migration timestamps are excluded, IDs/order/content are not |
| `public-headers.txt`, `public-symbols.txt` | byte-equal sorted path or ABI symbol/signature lines; addresses are omitted |
| `plugin-abi.json` | semantic sorted-key JSON; only build version is tokenized |
| `packages.txt`, `install-manifest.txt` | byte-equal sorted paths and package metadata; workspace prefix is tokenized |

`scripts/compare_surface_baseline.sh` uses only the named normalizers, exits 0 equal, 1 drift, and 2
invalid/missing baseline, and writes `build/reports/surface-diff.json`. A difference is accepted only
when the index entry names a separately approved compatibility record. Adding a normalizer is itself
a compatibility change. Generated inventories used by documentation are
`build/inventory/{dependencies,config-reads,surfaces,registrations,owned-data,tests}.json`; the Slice
0B generator and dependency scanner own them, and `scripts/check_module_docs.sh` compares them to
the descriptor, module evidence, and `docs/journeys/registry.yaml`.

### Deletion, migration, and recovery contract

Compatibility records require `id`, `owner`, non-owner reviewer attestation, effective release,
minimum supported release, affected surfaces/data, complete touch set, pre-change OCI image digest
and immutable release tag, migration-fixture paths, backup/export command, forward-fix command,
restore command, verification command, retention expiry, and rollback owner. Images and fixtures
remain available for the longer of two stable releases or 180 days after the effective release;
tags are `pre-core-refactor/<release>/<record-id>` and digest-pinned in the record. Release
engineering owns the registry retention check, and expiry cannot precede the compatibility window.

Schema additions must be readable or safely ignored by the retained prior binary throughout that
window. Destructive schema/data retirement is never bundled with source deletion: it requires a
separate append-only tombstone/copy migration and compatibility record after the window. The tested
recovery runbook is `scripts/recover_refactor.sh`: export with `scripts/export_db.sh`, apply the
record's forward-fix migration and verify; on failure, restore the versioned backup into a fresh DB
and run the retained digest-pinned binary's read-only verification. It never reverses an applied
migration in place.

Fixtures live at `tests/fixtures/db-compat/<release>/{db1,db2}.sqlite`. CI tests oldest-supported,
immediately-prior, and current empty databases; the new binary must open, migrate, preserve row
counts/keys/blobs, pass invariants, and be idempotent, while the prior binary must read data that
remains declared backward compatible. `scripts/check_deletion_dispositions.sh` resolves every touch
set path/ID through source, generated-source, symbol, route, schema, config, package, install, docs,
and test inventories before deletion, then requires each removed item to be absent from all current
inventories and descriptors. It also rejects a deletion whose touch set intersects any core
descriptor's `required_components` or the core-round-trip trace.

The temporary root allowlist is owned by the architecture reviewer, may only shrink, and any
addition requires its own approved exception with an expiry in the same PR.

## Roundtable review

The proposal completed three review passes on 2026-07-20:

1. Architecture, adversarial, and verification reviewers rejected the first draft for insufficiently
   concrete build generation, core-profile proof, feature-liveness evidence, deletion safety,
   dependency-cycle enforcement, descriptor completeness, surface baselines, and module-document
   gates.
2. The revised draft closed the structural issues but was rejected by architecture and adversarial
   review because several gates still described intent without normative schemas, targets,
   thresholds, equality rules, attestations, or recovery mechanics. Verification approved the
   direction.
3. The final draft added the normative contracts in this document. Architecture, adversarial, and
   verification reviewers independently found no remaining blocker and returned **APPROVED**.

Roundtable approval means the proposal is coherent and executable enough to seek project approval;
it does not bypass the per-slice human review, compatibility decisions, or acceptance gates defined
here.

## Move discipline

This program will touch thousands of include and build edges. The following rules make the large
refactor reviewable without reducing it to a directory shuffle:

- One ownership family per PR unless a shared seam makes two inseparable.
- Establish or narrow the public interface before moving callers.
- Use path-only moves only when the implementation already has one responsibility and one
  authoritative path. Otherwise split, consolidate, rename, and delete within the owning slice.
- Preserve public behavior by fixture/contract tests; private interfaces have no compatibility
  entitlement and should be simplified as soon as their callers are in the same slice.
- Run blast-radius analysis before each broad family move and update the module dependency graph
  in the same commit. Store the machine-readable result with the slice ledger and require a
  reviewer to acknowledge every high-confidence incoming edge.
- Preserve public CLI commands, HTTP/RPC routes, database schemas, config keys, plugin manifests,
  and wire shapes unless another approved proposal explicitly changes them.
- Never solve a dependency violation by adding a broad global header or by moving optional behavior
  into `base`/core.
- Remove feature-specific tests with a deleted feature. Do not keep a private subsystem alive solely
  to keep its self-tests green.
- Each PR includes a cleanup ledger: production additions, deletions, consolidations, remaining
  fallbacks, and why any net production growth is necessary.
- The ledger is `docs/refactor-ledgers/<slice>/<family>.yaml`, consumed by
  `scripts/check_cleanup_ledger.sh`. Every production addition names a current consumer and either
  the implementation it replaces or the reason net growth is necessary; unconsumed additions
  fail the gate.
- Each PR updates the owning module documents in the same change; path or symbol churn alone is not
  allowed to leave architectural documentation stale.
- A module that cannot yet be disabled may land as `optional, default_enabled: true,
  runtime_toggle: false`; the descriptor must state the remaining concrete dependency and a later
  slice removes it.

## Non-goals

- Rewriting C implementations in another language.
- Combining DB1 and DB2 into one database or changing tenancy/storage guarantees.
- Making all modules dynamically loaded shared libraries.
- Changing the public command or API taxonomy merely to mirror directories.
- Treating code intelligence, embedding, or reranking as optional. Individual language parsers and
  additional inference backends may be optional; the operations and one working implementation of
  embedding and reranking are mandatory.
- Moving generic code into a junk-drawer `common`, `misc`, or oversized `base` directory.
- Duplicating the IR-stage registry, provider registries, or plugin system.
- Preserving an unused feature merely because it has tests, docs, schema, or a registered surface.

## Risks and mitigations

- **Large refactors hide regressions.** Compare generated routes, CLI help, symbols, fixtures,
  database behavior, and end-to-end journeys against Slice 0B baselines; keep each ownership family
  independently revertible.
- **A false core grows until nothing is optional.** The four-capability thesis is the admission
  rule. A dependency used by optional features does not become core merely because it is popular.
- **A tiny core becomes unusable abstraction theater.** The core profile includes reference
  adapters, working embedding and reranking, and an executable
  write/embed/retrieve/rerank/route/IR/translation round trip without synthesis.
- **Memory becomes a new monolith.** Memory is one capability and ontology, not one compilation
  unit. Its modality, producer, storage, service, and CLI submodules remain separately owned.
- **Provider code blurs routing and translation.** Split selection, conversion, and transport at
  their contracts; do not choose ownership by filename.
- **Make/CMake drift.** One descriptor graph generates inputs for both and CI compares selected
  sources.
- **Runtime-disable claims outrun reality.** Physical modularity, build omission, and runtime
  unloadability are separately declared and separately tested.
- **Deletion meets deployed state.** Source/API deletions and persisted-data retirement are
  separate decisions. Applied migrations are never removed; pre-deletion images/tags and compatible
  schemas are retained through the window, and recovery after schema advancement is a tested
  forward fix or data restore—not an unsafe source revert.
- **Self-contained features look alive.** Liveness classification excludes test-only and self
  edges, requires a supported journey or non-self consumer for retention, and records unknowns
  instead of treating internal activity as utility.
- **LOC targets encourage code golf.** Deletion is evaluated with contract clarity, dependency and
  concept counts, and production-only ledgers—not raw LOC alone. Net production growth requires an
  explicit rationale, but readability is not sacrificed for a smaller number.

## Acceptance

- The repository and architecture documentation define Aimee Core as memory (including code
  intelligence, embedding, and reranking), routing, IR messaging, and translation. Synthesis is an
  optional module over recalled memory.
- `memory`, `routing`, `ir`, and `translation` are explicit mandatory source modules with public
  contracts and no dependency on optional modules.
- Code indexing, symbol/call/dependency graphs, architecture facts, and blast radius live under the
  memory owner and use the memory ontology/recall contracts.
- Every core profile resolves working embedding and reranking implementations and fails startup
  clearly if either is unavailable; synthesis can be absent without degrading ranked evidence
  retrieval.
- Every retained feature has a liveness classification, a non-self consumer or supported journey,
  and an explicit owner. Self-contained islands and superseded implementations are deleted with
  their tests/config/schema/docs rather than migrated.
- Feature implementation files no longer live at `src/` root. Only an explicit small allowlist of
  build/readme files remains there at completion.
- App directories own composition, listener, lifecycle, and generic dispatch only.
- Every optional built-in module has machine-readable metadata, declared dependencies, and a
  build/runtime availability state.
- Every core and optional module has an individual, authoritative document satisfying the module
  documentation contract and linked from the generated module catalog.
- Make and CMake consume the same module graph and reject cycles, undeclared edges, private-header
  imports, and core-to-optional dependencies.
- The core, personal, shared-KB, full, and full-minus-one profiles build and pass their relevant
  tests.
- Default-profile CLI help, route descriptors, API fixtures, plugin discovery, configuration, and
  database schemas remain compatible.
- Each slice reports its production cleanup ledger; duplicate paths, registries, fallbacks,
  internal representations, and pass-through layers trend downward, and any net production growth
  is explicitly justified.

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_module_deps.sh --catalog src/modules --ownership tests/baselines/modules/dependency-ownership.json --include-graph --public-symbol-graph --link-graph --no-cycles --no-core-to-optional --no-private-cross-imports --file-line-evidence"}
- {id: 2, tier: mechanical, check: "scripts/check_src_root_allowlist.sh src/ROOT_FILE_ALLOWLIST && scripts/check_forwarding_headers.sh --fail-migrated-consumers --enforce-expiry"}
- {id: 3, tier: mechanical, check: "make -C src module-profile PROFILE=core && make -C src module-profile-test PROFILE=core && cmake -S . -B build/core -DAIMEE_PROFILE=core && cmake --build build/core --target aimee-core-profile && cmake --build build/core --target aimee-core-test"}
- {id: 4, tier: mechanical, check: "scripts/test_module_profiles.sh --pr-tier --profiles core,personal,shared-kb,full --full-minus-one-every-optional --require-build-link-absence"}
- {id: 5, tier: integration, check: "scripts/compare_surface_baseline.sh --index tests/baselines/modules/index.yaml --write-report build/reports/surface-diff.json --cli-help --routes --ir-fixtures --config --db-schemas --public-symbols --public-headers --plugin-abi --packages"}
- {id: 6, tier: integration, check: "make -C src test-core-contracts test-memory-code test-ir test-routing test-translation"}
- {id: 7, tier: integration, check: "scripts/test_core_round_trip.sh --profile core --fixture tests/core_round_trip/core_round_trip.json --fixture-schema tests/core_round_trip/core_round_trip.schema.json --trace build/core/core-round-trip-trace.json --forbidden-symbols tests/core_round_trip/forbidden_synthesis_symbols.txt --require-all-seven-stages --require-synthesis-absent --min-cosine-margin 0.10 --min-distinct-l2 1e-6 --require-rerank-order m3,m1,m2 --min-ndcg 0.95"}
- {id: 8, tier: mechanical, check: "scripts/audit_feature_liveness.sh --cluster-schema docs/audit/feature-cluster.schema.json --disposition-schema docs/audit/disposition.schema.json --reviewers docs/audit/reviewers.yaml --exclude-test-edges --exclude-self-registration --require-non-self-endpoint-proof --fail-unassigned --fail-unclassified --fail-expired-unknown --fail-expired-exposed --fail-retained-self-islands --require-one-disposition-per-cluster --require-independent-review"}
- {id: 9, tier: mechanical, check: "scripts/check_cleanup_ledger.sh --require-every-slice --require-consumers --explain-production-growth --require-blast-radius"}
- {id: 10, tier: mechanical, check: "scripts/check_module_docs.sh --catalog docs/modules --descriptor-schema src/modules/module.schema.json --evidence-schema docs/modules/module-evidence.schema.json --journey-registry docs/journeys/registry.yaml --inventory build/inventory --required-sections --check-dependencies --check-config --check-surfaces --check-data --check-journeys --require-human-attestation"}
- {id: 11, tier: integration, check: "scripts/test_db_compat.sh --db1 --db2 --matrix oldest-supported,immediately-prior,current-empty --fixtures tests/fixtures/db-compat --compat-schema docs/audit/compatibility-record.schema.json --retention-policy two-releases-or-180-days --append-only-migrations --test-recovery scripts/recover_refactor.sh"}
- {id: 12, tier: mechanical, check: "scripts/check_generated_module_builds.sh --descriptor-schema src/modules/module.schema.json --make src/generated/modules.mk --cmake cmake/generated/modules.cmake --ownership tests/baselines/modules/ownership.json --all-profiles --byte-equal --fail-drift"}
- {id: 13, tier: integration, check: "scripts/check_plugin_abi.sh --baseline tests/baselines/modules/plugin-abi.json"}
- {id: 14, tier: mechanical, check: "scripts/check_deletion_dispositions.sh --schema docs/audit/disposition.schema.json --compat-schema docs/audit/compatibility-record.schema.json --inventories build/inventory --core-trace build/core/core-round-trip-trace.json --independent-approval --complete-touch-set --rollback-owner --deadlines"}
- {id: 15, tier: nightly, check: "scripts/test_module_profiles.sh --external-service-runtime --full-minus-one-every-optional && scripts/test_provider_readiness.sh --profile core --embedding memory.embedding.ready --reranking memory.reranking.ready"}
```
