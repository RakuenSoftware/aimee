# memory module

The public memory module packages a generic SDK/probe and an unavailable adapter. It owns no datastore, retrieval policy, ontology, maintenance, or source-release implementation. The bundled adapter exits before bus attachment and advertises no handler/stages. Selecting its executable is not serving readiness.

Use the existing role module manifest to select an independently installed implementation with exact executable grants. The selected implementation must honestly declare its supported wire capabilities; legacy stage IDs alone do not establish protocol compatibility. Aimee-specific memory behavior belongs in the memory module; reusable datastore behavior belongs in the independent database product.

The public server-go/memory SDK uses a borrowed admitted bus caller and remains independent of server domain storage. The generic probe lives at server-go/cmd/aimee-memory-bus-probe. Canonical source producers remain with DB1. Neither these producers nor the generic adapter silently implement the retired memory owner.

See [Memory boundary](../MEMORY.md) for qualification and private historical baseline details. No private implementation or database credentials are automatically installed by this repository.
