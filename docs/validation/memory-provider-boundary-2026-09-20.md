# Provider-bound memory and protected-context validation

A loopback provider capture on the real Server found two host serialization defects:

- Buffered `/v1/responses` flattened the input into a text prompt and discarded
  the separate `instructions` and tool definitions. The caller's memory payload
  arrived, while a separate user constraint did not.
- Chat/Responses ingress targeting Anthropic used the OpenAI backend builder,
  sending `role: system` messages and OpenAI tool schemas to `/v1/messages`.

Buffered Responses now parses structured messages, instructions and tools and
uses the same provider assembly and response governance path as streaming
Responses. The existing local text continuation remains available; current
instructions are supplied separately. Tool calls are relayed to the caller and
are not executed by this endpoint. Anthropic targets now use the Anthropic
serializer, including its separate system blocks and `input_schema` tool fields.
These are external host/translation fixes. The memory owner and its module-side
transport remain Go, and the C bus is unchanged.

`tests/e2e/memory-provider-boundary-e2e.py` stores and recalls a synthetic record
through the real Go private memory owner, then carries that returned projection
through Chat Completions, Responses and Messages ingress into both OpenAI-chat
and Anthropic provider formats. It checks Unicode/escaping, the complete memory
projection, a separate user constraint, tool schemas, continuation and returned
tool-call identities/arguments. Only the external provider is a fixture; it
listens on container loopback and never calls a real model. The test restores the
model roster and retires its memory. Deployment-matrix T2 and T3 now run it.

Captured accounting describes the exact HTTP request body consumed by the
fixture: UTF-8 byte count and SHA-256, without credentials, headers or retained
prompt bodies. The memory projection byte count is its pre-provider UTF-8 text;
provider escaping and wrappers are included in the full request byte count.
Token-count provenance is explicitly unavailable. Fixture response usage values
are synthetic and are not token-count or cost evidence. Commitments alone cannot
reconstruct requests or certify durable preparation/dispatch acknowledgement.

## Local checks

The native IR serve test verifies Anthropic system placement, message roles,
tool schema and output-token control. IR/legacy parity covers eight client
shapes. The governed Responses tool-output suite covers namespaces, argument
fidelity and Unicode. These tests and the complete application image build pass;
Go ownership/boundary checks and the 14 frozen semantic-context tests also pass.

Full provider token limits, protected overflow handling, bounded repacking,
source-version release binding and durable receipts remain open MR-03/MR-06
work. No new whole-request P95 or cost improvement is claimed.

## Fresh environment evidence

Exact source and harness revision `1f25b57f67` passed **760/760 checks** in
fresh owned deployments on `.253` (CT 9498):

- Enrolled T2: **490/490**, including 60 provider-boundary checks.
- Standalone T3: **270/270**, including 60 provider-boundary checks.

[Named T2 checks](memory-shared-reliability-2026-09-20/fresh-t2-1f25b57f67.json),
[named T3 checks](memory-shared-reliability-2026-09-20/fresh-t3-1f25b57f67.json)
and [captured accounting commitments](memory-shared-reliability-2026-09-20/provider-accounting-1f25b57f67.json)
retain the verdicts and counts without prompt bodies. Existing private/shared
review, retry, restart, outage, semantic and exploratory checks all pass.

Inspected image identities across all three placements:

- Application: `sha256:45cb9653b9ca4a3cd12e5ab649d914a1d90e8e76c72c7fa944fc3cbbb07ca149`.
- PostgreSQL: `sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
- Embedder: `sha256:f1286af7de10cf058a9bec14c45326d64de73e3878db19c732db86cda1f9d979`.

Raw receipts remain in `/opt/aimee-memory-proposals-evidence/` under
`t2-1f25b57f67-r2` and `t3-1f25b57f67-r2`. Initial attempts failed before
application startup because Docker exhausted its network address pools; removing
this task's stopped fixture containers and empty networks allowed fresh reruns.
Their retained volumes and evidence were preserved.

These captures exercise caller-supplied projections returned by the Go owner.
They do not certify automatic workspace ingress, hard token limits, durable
provider acknowledgements, or complete MR-03/MR-06 acceptance.
