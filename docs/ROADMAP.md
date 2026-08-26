# Roadmap

This is the remaining direction after 0.4.0. Accepted implementation detail lives in proposals;
current behavior lives in product guides and generated references.

## 1. Finish the event-bus move

- route workflow trigger firings through the bus;
- land external inline client attachment with admission and lifecycle proof;
- move the remaining inter-module observability paths off private callbacks;
- add uniform pre-delivery policy for action-class events;
- expose bounded telemetry without granting another full-stream observer;
- keep module replay separate from observational capture.

## 2. Complete module ownership

- finish source-module decomposition and public headers;
- make descriptors own dependencies, config, routes, capabilities, and event kinds;
- remove retired C workflow code and arbitrary plugin-loader residue;
- keep C/Go conformance and one writer during every migration.

## 3. Continue the Go service cutover

- move remaining DB1 API families behind one Go owner at a time;
- isolate provider, policy, vault, and tool resources before changing ownership;
- preserve `/v1` compatibility and crash recovery;
- keep native code only where the boundary and evidence justify it.

## 4. Close distributed trust

- finish per-user remote-write rollout and grant tooling;
- harden mTLS enrollment on macOS and Windows;
- complete external witness/anchor operations and operator evidence surfaces;
- keep egress, budgets, catalogs, and identity consistent across server and KB.

## 5. Recovery and operations

- transactional turn rewind and browser recovery;
- appliance state recovery runbooks;
- config descriptor and route descriptor completion;
- repeatable restore, scale, and failure-injection gates;
- honest health for every optional KB model role or sidecar dependency.

## 6. Knowledge breadth

- organization data connectors with scope and provenance;
- fleet registration and routing across multiple KBs by corpus, authority, and capabilities;
- per-KB internal or remote embedder and synthesizer placement, with no separate inference service;
- curator extraction quality and benchmark cadence;
- retrieval fusion selection and evidence contracts;
- better code-graph architecture surfaces;
- corpus-scale indexing without weakening scope or citation.

See [Proposals](PROPOSALS.md) for current design records and [Feature status](STATUS.md) for what is
integrated.
