# KB fleet and model placement

The target deployment has one or more `aimee-kb` containers. Each KB owns its storage boundary and
declares which model roles it can serve. An optional model-specific `aimee-llm` sidecar can execute
local synthesis for one KB. It is part of that KB's placement and is never a server routing target.

## Model roles belong to a KB

Each KB can configure the embedding and synthesis roles independently:

| Role placement | Meaning |
| --- | --- |
| local | Embedding runs in the KB image or selected embedder sidecar; synthesis runs in the selected `aimee-llm-*` sidecar. |
| remote | The KB calls an explicitly configured remote model endpoint. |
| off | The role is unavailable and dependent stages report degradation. |

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

Several stateless KB replicas may share an explicitly configured DB2 when they have the same storage
and schema authority. Separate corpora or trust boundaries use separate DB2 ownership. A deployment
must not infer either arrangement from container names.

## Current implementation boundary

The managed and split Compose profiles in this checkout deploy one named `aimee-kb`, and the server
uses one `AIMEE_KB_API_URL`. They are the single-KB profile of the design. Fleet registration,
selection, and multi-KB operator commands are not yet an integrated path, so current guides do not
invent commands for them.

Until that path lands, scale identical KB workers only where the shared DB2, identity, and queue
contracts already support it. Do not present independent KBs as one fleet by placing a generic load
balancer in front of them; that would erase the routing authority described above.

See [Architecture](ARCHITECTURE.md), [KB model backends](KB_LLM_BACKENDS.md), and
[Feature status](STATUS.md).
