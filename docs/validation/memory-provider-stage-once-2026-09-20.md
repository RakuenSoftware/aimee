# One memory stage at final provider assembly

Final provider captures showed buffered Responses requests carried an additional
copy of standing guidance compared with the same Chat request. Both live
Responses handlers called `aimee_ir_responses_to_chat`, which applied request
stages, and then `agent_execute_messages` rebuilt the provider request through
`aimee_ir_build_from_chat`, which applied those stages again. This repeated the
Go memory context/tool plans, recall and guidance. The early persona insertion
also contaminated the query captured by the second recall.

The intermediate Responses conversion now only decodes structured input. Final
provider assembly retains the existing stage order after routing. Native
regressions first failed on the old early context-plan call, then passed with
zero stage calls during decode, one context/tool-plan call during assembly,
the pristine user query, and one persona in OpenAI, Anthropic and Responses
provider bodies. IR/legacy parity still covers all eight supported client shapes.

The real Server capture fixture now covers streaming Responses as well as
buffered Responses, Chat and Messages. It verifies exactly one standing guidance
block for both Responses modes and Chat, while retaining existing complete
Go-recalled memory, Unicode, separate constraints, tools, continuation and tool
relay checks. Exact final request byte counts are fixture evidence, not provider
token counts or a whole-request latency benchmark.

Local validation: native IR serve and IR/legacy parity tests, memory ownership
and C boundary checks, and 17 semantic-context baseline/evidence tests pass.
The first live run on `ee89d33916` failed the stronger guidance check: Chat
and both Responses modes still carried two copies. A diagnostic capture traced
the second copy to the host-composed persona, which already includes the exact
standing guidance. Those failed receipts remain under `t2-ee89d33916` and
`t3-ee89d33916` on CT 9498; they are not counted as passing releases.

The generic host plan connection now reports names of resources inserted by that
assembly. Only an actually inserted host-composed persona supplies `guidance`;
caller-provided persona markers or text do not. The Go memory plan uses this fact
to omit duplicate guidance while retaining recall, evidence and tool handling.
Go regressions cover absent/empty/unknown resource lists, host-supplied guidance,
malformed lists and untrusted lookalike text. Native tests verify exact resource
forwarding and host insertion facts. Memory selection remains Go and the C bus
is unchanged.

Buffered Chat also defers its legacy pre-injection/persona work until the plain
text branch, preserving that branch's response-cache identity. Structured requests
use final IR assembly; the explicitly disabled IR route retains its legacy
context fallback. This avoids an unused recall and consuming a persona delivery
claim for a prompt that the structured route discards.

The full required PostgreSQL-backed memory/family race suites pass (53.655 and
1.441 seconds); native IR serve, IR/legacy parity, generic host/Go plan, ownership,
C boundary and 17 semantic-context tests pass. The Go economizer race suite also
passes for the separately documented request-digest correction, which is included
in the same candidate image.

The temporary persona instructions are also freed after the IR helper copies
them, fixing an existing allocation leak. Native IR serve/parity pass again.

The intermediate `9bfa761cf6` fresh runs exhausted the disposable container's
120 GiB filesystem before provider validation. These are environment failures,
not passing application receipts. Stopped task containers/networks and redundant
source archives were removed; database volumes and evidence were retained. The
host storage pool had more than 3 TiB free, so the task-owned CT 9498 root disk
was expanded to 184 GiB before the final exact-revision fresh run.

## Fresh exact-revision results

Application and harness `928919a6ea` passed **800/800 checks** on `.253`:

- Enrolled T2: **510/510**, including 80 provider-boundary checks.
- Standalone T3: **290/290**, including 80 provider-boundary checks.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-928919a6ea.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-928919a6ea.json)
and [provider accounting commitments](memory-shared-reliability-2026-09-20/provider-accounting-928919a6ea.json)
retain no prompt bodies or headers. Raw results remain under
`/opt/aimee-memory-proposals-evidence/t2-928919a6ea` and `t3-928919a6ea`.

For the same recalled projection, constraint and tool in each topology, Chat,
buffered Responses and streaming Responses produce identical provider bytes and
SHA-256 digests:

| Placement | OpenAI chat body, each client | Anthropic body, each client |
|---|---:|---:|
| T2 | 8,095 bytes | 8,197 bytes |
| T3 | 7,725 bytes | 7,827 bytes |

The previous `1f25b57f67` captures had 2,196 additional OpenAI bytes and 2,170
additional Anthropic bytes for buffered Responses relative to equivalent Chat
input in each topology. That frontend-specific excess is now zero. This is a
matched wire-size comparison, not a token-cost, recall-quality or P95 measurement.

All nine containers were inspected against these image identities:

- Application: `sha256:24088350ea818426ae811450ffb8ab4fb1bcfe0b6b65596b0d9d784f79e2a236`.
- PostgreSQL: `sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
- Embedder: `sha256:f1286af7de10cf058a9bec14c45326d64de73e3878db19c732db86cda1f9d979`.

Provider captures still exercise caller-supplied Go recall projections. They do
not certify automatic workspace ingress, production hard token caps, source-version
release binding, protected overflow or durable provider acknowledgement. Full
MR-03/MR-06 acceptance remains open.
