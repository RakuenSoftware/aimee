# Proposal: A Python module that seats LangGraph graphs on aimee's memory and delegates

- **State:** PENDING. Design only, no code in this PR.
- **Author:** JBailes
- **Date:** 2026-08-29
- **Charter roles:** Execute (graph nodes dispatch through the existing
  delegate-invocation stage), Persist (graph reads and writes cross the memory
  module's write and recall gates), Review (the module adds no authorization
  path of its own; it consumes the ones that exist).

## Thesis

LangGraph is good at graph execution and has an ecosystem. Its durable state is
a checkpointer: a per-thread snapshot that exists to resume a run. It has no
extraction, no contradiction detection, no decay, no PII recall gate, and no
confidence tier. Bolting a vector store on does not produce those.

aimee has all of them behind five bus stages and no reach into the frameworks
people already run.

A Python module trades one for the other. It is not new plumbing: `memory`
already serves `write` 5890, `extract_index` 5889, `retrieve` 5892, `reranking`
5893 and `embedding` 5891 to a supervised Go process, and `delegates` already
serves a version-2 execution contract over `delegate-invocation` to the native
WFE and roundtable. Both consumers are in-tree and neither is C. The module adds
a second consumer of two contracts that already have one.

The claim is reach, not capability. Nothing here lets aimee do something it
cannot do today. It lets a graph someone already wrote do it.

## The case for not doing this

Two objections deserve their strongest form.

**The dependency tree.** aimee vets untrusted MCP packages and ships delegate
sandboxes with no network and no credentials. LangChain's transitive
dependencies are large and move fast. Putting that in the server path would be a
governance event, and calling it a module does not change what it pulls in.

The answer is placement, not assurance. The module is the process boundary. It
holds no vault material, resolves no workspace, and reaches storage only through
the stages below. Its blast radius is the grant its principal ref carries, which
is the same containment argument every other module already makes. A deployment
that does not want it sets `AIMEE_MODULE_LANGGRAPH=0` and the seam is absent
rather than degraded.

**Two orchestrators, two behaviours.** If the module owns tool loops, aimee has
two of them. Two tool loops that derive arguments differently mean
execution-policy authorizes different shapes depending on which one ran, and the
ledger records calls that did not execute.

That was true when this was first sketched. It is now false: argument shape is
decided once, in `tool_args_canonicalize` (`headers/tool_args_coerce.h`), before
any gate, and `src/posix/agent_runtime.c` rewrites the parsed call in place so
every reader sees the same bytes. A second orchestrator calls the same function
and cannot diverge. The precondition is met; that is why this proposal is
reachable now and was not before.

## What the module owns

Graph definition, graph execution, and the translation between a LangGraph node
and a bus call. Nothing else.

It does not own authorization, credentials, workspaces, worktrees, storage, or
the audit path. A node that wants any of those makes a bus call and gets the
answer the module that owns it returns, including that module's failure
semantics: the typed-fact write gate defers rather than committing, and both
halves of the PII recall gate withhold rather than expose. Inheriting those is
the point. A LangGraph node talking to Postgres directly has none of them.

## State: two stores, one of them not memory

The tempting design is to make aimee memory the checkpointer. One store, one
answer to "what does this graph know."

It is the wrong split, and the write gate is what shows it. A checkpoint is raw
per-thread state: messages, node outputs, pending writes, retained so a run can
resume. Curated memory is facts with evidence and decay. Routing checkpoints
into `write` 5890 asks a gate whose job is deciding what may commit as a
semantic edge to adjudicate transcript noise, at every superstep. It will
correctly refuse most of it, and the refusals are not a signal anyone can act on.

So:

- **Resumption state** stays mechanical. `lifecycle_work_item` if the run is a
  work item, LangGraph's own checkpointer otherwise. Uninteresting by design.
- **Memory** is a node capability: recall through `retrieve` 5892 with the tier
  from `reranking` 5893, writes through `write` 5890.

The awkward consequence is that a graph has two places state can live and the
module cannot decide which for the author. A node that stashes a summary in
checkpoint state instead of writing a fact gets no curation, no decay, and no
recall from another session, and nothing in the API stops it. That is a
documentation and example burden the module carries permanently.

## Attaching from Python

The bus is not JSON over a socket. `src/core/README.md`: modules attach through
a local `SOCK_SEQPACKET` handshake and then use only their shared-memory
mappings, and external module repositories link the
`aimee-core-event-bus-client` archive, which carries no host or routing code.

So the module is a cffi binding over that archive plus graph code on top. The
archive already exists and was already carved out for out-of-tree consumers, so
this is binding work rather than protocol work. It is the only genuinely new
engineering in the proposal and the only line item I would not estimate from the
existing modules, because every current provider is C or Go and none of them
exercises the client archive from a foreign runtime.

The rest is known cost:

- A principal ref from `tests/baselines/modules/canonical-inventory.yaml`. Event
  kinds are carved as `4096 + ref*256 + stage`, and a ref is retired rather than
  recycled, so allocation is one-way.
- Registration in Make and CMake, a `module.yaml` descriptor, test registration
  pinned to `tests/baselines/refactor/module-test-registration.json`, and a
  canonical document under `docs/modules/`.
- Image packaging and supervision for a Python runtime, which
  `module-runtime` has not supervised before.

## Staging

1. **Attach.** cffi binding over the client archive, one round trip against a
   stage that already exists, no LangGraph. This retires the only unknown, and
   it is worth building before the ref is allocated.
2. **Memory as a node capability.** Recall and write through 5892/5890/5893,
   with the gate failure semantics surfaced as node errors rather than swallowed.
3. **Delegates as nodes.** A node that dispatches through `delegate-invocation`
   and returns the bounded result.
4. **Optional toggle and docs.** `AIMEE_MODULE_LANGGRAPH`, the module document,
   and the examples that teach the state split above.

Stage 1 is the decision point. If attaching from Python is awkward, that is
worth knowing before the ref is spent and before any LangGraph code exists.

## What would make this the wrong call

If the graphs people actually bring turn out to need aimee's worktrees and
write-authority model rather than just memory and a delegate call, the module
becomes a second front door onto machinery whose contracts assume the WFE owns
the run. The seam to watch is source authority: a graph node that wants a
writable worktree is asking for something `delegate-invocation` deliberately
keeps caller-side. If that request arrives early and often, the answer is
probably to keep those graphs outside and let them call in, not to widen the
module.
