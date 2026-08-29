# LangGraph module contract

Design for the Python module proposed in
[langgraph-module-with-memory-and-delegate-interop](../proposals/pending/langgraph-module-with-memory-and-delegate-interop.md).
The proposal argues for it; this page is the contract someone would build
against. Nothing here is implemented yet, and the event kinds below are
derivations, not allocations.

## Boundary

The module owns graph definition, graph execution, and the translation between a
LangGraph node and a bus call.

It owns no authorization, credential, workspace, worktree, storage or audit
path. Every governed action is a bus call to the module that owns it, and the
answer, including a refusal, is returned to the node unchanged.

## Principal and event kinds

A ref is allocated in `tests/baselines/modules/canonical-inventory.yaml` under
`principal_refs` and added to `retired_principal_refs` if the module is ever
removed. Event kinds derive from it as `4096 + ref*256 + stage`; they are not
chosen.

The module serves one stage. A graph run is a request in and a bounded result
out, so a second stage would only split what callers already treat as one
operation.

| Stage | Purpose |
|---|---|
| `run` (stage 1) | Execute a named graph against a payload, return the result and a per-node trace. |

Everything else the module does is outbound: it consumes other modules' stages
rather than serving more of its own.

## Stages the module consumes

| Module | Event | Used for |
|---|---|---|
| `memory` | `retrieve` 5892 | Node recall. Returns the PII sensitivity tier per relation; a node cannot bypass it by not asking. |
| `memory` | `reranking` 5893 | The `high`/`medium`/`low` confidence band attached to recalled context. |
| `memory` | `write` 5890 | Node fact writes. The typed-fact gate decides whether a candidate triple may commit as a semantic edge. |
| `memory` | `embedding` 5891 | Only if a graph needs its own vectors. Most will not; recall covers the common path. |
| `delegates` | `delegate-invocation` (v2) | A node that dispatches a bounded delegate turn and returns its result. |

`extract_index` 5889 is deliberately absent. Extraction is the curator's job on
the turn stream, not something a graph node should trigger.

## Failure semantics the module inherits

These are chosen per seam by the owning module and the node sees them as errors,
never as empty success.

| Seam | On failure |
|---|---|
| `write` 5890 | Defers. Nothing is written and the caller may retry. A node must not treat a deferral as a successful write. |
| `retrieve` 5892 | Fails closed. Recall withholds rather than exposes. |
| `reranking` 5893 | No local substitution. A missing tier means the context envelope is omitted and the absence is logged, not that the tier is `low`. |
| `delegate-invocation` | A malformed handoff is a verdict, not a transport failure. |

A node that swallows any of these and continues is the module's bug, not the
seam's. Surface them.

## State

Two stores, and the split is load-bearing.

**Resumption state** is mechanical: `lifecycle_work_item` when the run is a work
item, LangGraph's own checkpointer otherwise. Messages, node outputs and pending
writes live here.

**Memory** is a node capability, not the checkpointer. Routing checkpoints
through `write` 5890 asks a gate that decides what may commit as a semantic edge
to adjudicate transcript noise at every superstep. It will refuse most of it,
correctly, and the refusals carry no signal.

The cost is that a graph author can stash a summary in checkpoint state instead
of writing a fact, get no curation, no decay and no cross-session recall, and
see no error. Examples must teach the difference; the API cannot enforce it.

## Attaching from Python

The bus is a POSIX shared-memory bus, not JSON over a socket. Modules attach
through a local `SOCK_SEQPACKET` handshake and then use only their shared-memory
mappings (`src/core/README.md`).

Out-of-tree modules link `aimee-core-event-bus-client`, which carries the
attach, wire, region and ring contracts and no host-side region lifecycle,
admission, routing, arena allocator or capture code. The Python module is a cffi
binding over that archive.

Build it and prove one round trip against an existing stage before allocating a
principal ref. Every current provider is C or Go, so this is the one step whose
cost cannot be read off an existing module.

## Argument shape, if the module runs tool loops

A node that calls a delegate does not run a tool loop; the delegate does. A node
that calls a model directly and dispatches tools itself does.

In that case it must use `tool_args_canonicalize`
(`src/headers/tool_args_coerce.h`) and authorize the canonical bytes, not the
model's raw arguments. The C loop rewrites the parsed call in place before any
gate runs, so the two orchestrators produce identical shapes. Deriving arguments
independently reintroduces the defect that fix closed: execution-policy
authorizing one shape while the tool runs another, and an audit record of a call
that never executed.

## Activation

`AIMEE_MODULE_LANGGRAPH=1` or `0` overrides whatever the image shipped; unset
keeps it. The module is optional: a deployment without it is a smaller
deployment, not a broken one, which is the test for `runtime_toggle.supported`.

## Registration checklist

- Principal ref in `canonical-inventory.yaml` (one-way).
- Sources and tests in `module.yaml`, with `ownership_complete` set only once the
  latch actually holds.
- Build registration in Make and CMake.
- Test registration regenerated into
  `tests/baselines/refactor/module-test-registration.json`.
- A canonical document at `docs/modules/langgraph.md` covering purpose,
  contracts, dependencies, providers, configuration, surfaces, data, security,
  journeys, tests, diagnostics, compatibility, and removal.
- Python runtime in the server image and under `module-runtime` supervision.
