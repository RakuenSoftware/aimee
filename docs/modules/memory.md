# memory module

The public memory module packages a generic SDK/probe and an unavailable adapter. It owns no datastore, retrieval policy, ontology, maintenance, or source-release implementation. The bundled adapter exits before bus attachment and advertises no handler/stages. Selecting its executable is not serving readiness.

Use the existing role module manifest to select an independently installed implementation with exact executable grants. The selected implementation must honestly declare its supported wire capabilities; legacy stage IDs alone do not establish protocol compatibility. Aimee-specific memory behavior belongs in the memory module; reusable datastore behavior belongs in the independent database product.

The public server-go/memory SDK uses a borrowed admitted bus caller and remains independent of server domain storage. The generic probe lives at server-go/cmd/aimee-memory-bus-probe. Canonical source producers remain with DB1. Neither these producers nor the generic adapter silently implement the retired memory owner.

See [Memory boundary](../MEMORY.md) for qualification and private historical baseline details. No private implementation or database credentials are automatically installed by this repository.

Deployment compatibility tests can explicitly select the actual historical Go owner as an external reference fixture. [Its frozen public source pin](../../tests/fixtures/legacy-memory-owner.json) is separate from the shipped adapter. With Go 1.26.10 available, run `python3 scripts/build_reference_memory_fixture.py --output /absolute/test-cache/aimee-module-memory` from the repository root; create the output directory first. The builder exports the pinned source into a disposable directory outside the checkout and writes a fresh executable plus a `.json` provenance sidecar recording its source commit and binary SHA-256.

This reference is not installed or shipped by the builder. Deployment tests select it through an exact executable grant at `/opt/aimee-test-memory-reference/aimee-module-memory`; other multicall roles retain their shipped executables. Its historical SQL behavior does not establish readiness or parity for the default unavailable adapter.
