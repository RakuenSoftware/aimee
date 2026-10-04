# Roadmap

This is the remaining direction around the 1.0.0 release target. Accepted implementation detail lives in proposals;
current behavior lives in product guides and generated references.

## 1. Finish the event-bus move

- route workflow trigger firings through the bus;
- qualify a general external-client API beyond current executable-bound module attachment;
- move the remaining inter-module observability paths off private callbacks;
- add uniform pre-delivery policy for action-class events;
- expose bounded telemetry without granting another full-stream observer;
- keep module replay separate from observational capture.

## 2. Complete module ownership

- continue moving remaining C resource/domain logic behind the existing checked module contracts;
- keep the 34 canonical descriptors, process contracts, owned files and exported builds synchronized;
- remove retired C workflow code and arbitrary plugin-loader residue;
- keep C/Go conformance and one writer during every migration.

## 3. Continue the Go service cutover

- keep the completed DB1 Go-family catalog, PostgreSQL transport, and migration ownership in sync;
- preserve the completed Go provider preparation and governed egress paths while migrating remaining resource mechanics;
- preserve `/v1` compatibility and crash recovery;
- keep native code only where the boundary and evidence justify it.

## 4. Close distributed trust

- finish per-user remote-write rollout and grant tooling;
- harden mTLS enrollment on macOS and Windows;
- complete external witness/anchor operations and operator evidence surfaces;
- keep egress, budgets, catalogs, and identity consistent across server and KB.

## 5. Recovery and operations

- transactional turn rewind and browser recovery;
- rehearse the existing appliance recovery runbooks against new failure modes;
- config descriptor and route descriptor completion;
- repeatable restore, scale, and failure-injection gates;
- honest health for every optional KB model role or sidecar dependency.

## 6. Knowledge breadth

- organization data connectors with scope and provenance;
- fleet registration and routing across multiple KBs by corpus, authority, and capabilities;
- fleet routing across per-KB embedding placement and local-sidecar or remote synthesis placement;
- curator extraction quality and benchmark cadence;
- qualify optional retrieval selection and utility policies before enabling them;
- better code-graph architecture surfaces;
- corpus-scale indexing without weakening scope or citation.

See [Proposals](PROPOSALS.md) for current design records and [Feature status](STATUS.md) for what is
integrated.

The generic memory contract and native/Cognee integration are implemented for the 1.0.0 work
in [PR #3005](https://github.com/RakuenSoftware/aimee/pull/3005). Remaining memory-engine work is
larger-corpus pagination/indexing and additional provider qualification, not a second authority
or module infrastructure. Node unification and parent connections require a separate design.
