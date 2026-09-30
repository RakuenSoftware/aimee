# KB fleet and model placement

Server works independently with its own personal store and models. A shared KB is optional and
separately deployed. See [Server and KB](SERVER_AND_KB.md).

The future fleet topology has one or more `aimee-kb` instances. Each KB owns its storage boundary and
declares which model roles it can serve. An optional model-specific `aimee-llm` sidecar can execute
local synthesis for one KB. It is part of that KB's placement and is never a server routing target.

## Shared-corpus model roles belong to their KB

Each KB can configure the embedding and synthesis roles independently:

| Role placement | Meaning |
| --- | --- |
| local | Standard Compose runs embedding in a separate sidecar; synthesis runs in an optional selected `aimee-llm-*` sidecar. |
| remote | The KB calls an explicitly configured remote model endpoint. |
| off | The role is unavailable and dependent stages report degradation. |

Server configures its own embedding and optional synthesis for personal data. Connecting a KB
does not replace those roles or send personal vectors into the shared corpus.

A KB may serve embedding, synthesis, or both roles. Local placement does not create a
model service for the server to route to. Remote placement does not move role ownership
to the server; the KB still owns admission, credentials, health, and the request contract.

## Fleet routing

The server must select a KB that is valid for the request's tenant, scope, storage authority, and
required capabilities. It must not send a request directly to a model runtime or silently substitute
a KB with a different corpus or vector-space identity.

Every routed result needs to preserve:

- the selected KB identity;
- tenant, team, project, and scope authority;
- the embedding model, dimension, pooling, and prefix identity for vector operations;
- the synthesis model and egress policy when synthesis runs;
- honest role health and degradation;
- request and audit correlation across the server-to-KB boundary.

Several stateless KB replicas may share an explicitly configured knowledge store when they have the same storage
and schema authority. Separate corpora or trust boundaries use separate knowledge store ownership. A deployment
must not infer either arrangement from container names.

## Current implementation boundary

The standard and managed Server Compose profiles install no KB. A distinct KB Compose project
can be enrolled later, and Server uses one configured `AIMEE_KB_API_URL`. Fleet registration,
selection, and multi-KB operator commands are not yet an integrated path, so current guides do not
invent commands for them.

Until that path lands, scale identical KB workers only where the shared knowledge store, identity, and queue
contracts already support it. Do not present independent KBs as one fleet by placing a generic load
balancer in front of them; that would erase the routing authority described above.

See [Architecture](ARCHITECTURE.md), [KB model backends](KB_LLM_BACKENDS.md), and
[Feature status](STATUS.md).
