# The embedder as a sidecar, and model changes as supported operations

- **State:** DONE — sidecar substrate delivered and archived 2026-08-04; deployment/migration residual extracted.

> **Archived after partial delivery.** Two baked embedder images, their guarded publish workflow,
> stunnel mTLS proof, KB-issued sidecar identity, serving-identity drift checks, and curator replay on
> model changes are present. Managed deployment still embeds in the KB image, and a same-dimension
> vector-space change still requires a fresh DB. The remaining cutover is
> [`embedder-sidecar-deployment-and-migration-residual.md`](../pending/embedder-sidecar-deployment-and-migration-residual.md).

Raised 2026-08-03 after CI was
rate-limited by Hugging Face four times in one evening; the question of why turned into
a design one.

## What was decided

The embedder becomes a **deployable sidecar container**, in three flavours:

| deployment | what runs | embedder resolved by |
| --- | --- | --- |
| external embedder | *no embedder container* | `EMBEDDER_URL` |
| bundled, small | `aimee-embedder-a25m` | the sidecar |
| bundled, wider | `aimee-embedder-nomic` | the sidecar |

and two model changes become supported, signposted operations rather than dead ends:

- **switching the embedder container** → drop the old kb data, re-index, re-embed,
  re-synthesise
- **switching the synthesis model** → drop the old synthesis output and re-synthesise

## §1 This reverses a documented decision, deliberately

`Dockerfile` currently says:

> The kb embeds ITSELF now. There is no embedder sidecar and no aimee-llm hop on the
> retrieval path: embedder-server.py runs on loopback inside this container with the
> weights BAKED IN … This deliberately reverses the split the unified-llm cutover
> introduced.

That comment must be rewritten, not left to contradict the code. The reasons for going
back to a sidecar are different from the reasons it was retired:

- **The kb image stops fetching from Hugging Face at all.** Not "less often" — the kb
  build has no model step left. The embedder weights move to a tag whose inputs change
  on the order of never, exactly as `aimee-model-*` did for synthesis.
- **The kb image loses CPU torch and the weights**, roughly a gigabyte, from every
  install including the ones using an external embedder.
- **The embedder becomes swappable as a deployment action** rather than a rebuild.

**The cost, stated plainly:** an HTTP hop lands back on the retrieval path, and
embedding happens on every search, not only on ingest. That is precisely what the
retirement comment objected to. The mitigation is that the hop is a sidecar on the same
host over loopback-equivalent networking, the same shape as `aimee-llm`, and the
embedder is small — but this is a latency regression on the hottest path in the product
and should be measured, not assumed. If it does not hold up, the honest fallback is the
build-input image (weights baked, no hop), which gets the Hugging Face win and not the
swappability.

## §2 Shape of the sidecar

**Both model roles are sidecars; synthesis already is one.** This is not two designs, it
is one pattern with two instances, and the embedder is the instance that has not caught
up:

| role | sidecar | port | external instead | selected by |
| --- | --- | --- | --- | --- |
| synthesis | `aimee-llm-e2b` / `-e4b` | 8761 | `SYNTHESIS_ENDPOINT` | `synthesis_model` |
| embedding | `aimee-embedder-a25m` / `-nomic` | 8762 | `EMBEDDER_URL` | `embedder_model` |

Every property of the synthesis hop should hold for the embedder one, and the ones that
were expensive to learn are worth naming so they are not re-learned:

- the kb owns the CA and issues the sidecar's server certificate, because the kb is the
  client on the hop (`kb_synthesis_identity_ensure` is the model)
- the client must *present* its certificate — `agent_http`'s default context presents
  nothing, which is #2284, and it made synthesis fail silently for an entire deployment
- the identity is loaded lazily on first use, because it does not exist yet when
  `agent_http_init()` runs (#2276)
- a missing identity is an error the sidecar reports, not something it waits out
- the sidecar's healthcheck needs a `start_period` that covers loading the model, or
  `depends_on: service_healthy` fails a deployment that is merely slow (#2272)

Follow `aimee-llm`, which is the working precedent in this tree:

- `Dockerfile.embedder`, one model per tag, weights baked, `BUILDPLATFORM`-pinned fetch
  so a two-architecture build downloads once.
- Terminated by stunnel with `verifyChain = yes`; the embedder binds loopback inside its
  own namespace and is reachable only through the terminator. The kb owns the CA and
  issues the sidecar's certificate, as it already does for synthesis
  (`kb_synthesis_identity_ensure`) — this needs the equivalent for the embedder, and the
  client must present its certificate, which is the bug fixed in #2284. Do not
  re-discover that: `agent_http`'s default context presents nothing.
- Deployment order: server, wizard, kb, then the sidecars. A missing identity is an
  error, not something to wait out.
- `EMBEDDER_URL` unset and no sidecar selected is now a real configuration (external),
  so "no embedder" must stop meaning "lexical fallback, silently".

## §3 What each switch invalidates, measured

### Embedder change

`db2_reembed.c` already drops and recreates the ten derived vector tables —
`kb_embeddings`, `kb_pdf_embeddings`, `memory_embeddings`,
`curator_{entity,narrative,claim,code_unit}_vectors`, `exemplar_vectors`,
`evidence_vectors`, `code_embeddings` — and refuses if it meets a halfvec table it does
not recognise rather than risk destroying source it does not understand. Each rebuilds
from an authoritative source, so no source data is lost.

Then it calls `db2_curator_reembed_all()`:

```sql
UPDATE artifacts SET state = 'proposed' WHERE state = 'committed'
  AND kind IN ('doc_summary','synthesis','open_question','claim','entity','code_unit')
```

**That is re-indexing, not re-synthesis.** The artifacts keep the text the old model
produced; only their vectors are rebuilt at the new width. So the re-synthesise half of
this requirement does not exist yet.

Which stages actually need regenerating is worth being precise about:

- **Per-chunk stages are embedder-independent.** `extract_doc` receives
  `input.content` directly (`CE_EXTRACT_DOC_PROMPT`), so its claims and entities are a
  function of the document text alone.
- **Cross-document stages are not.** `synthesize`, `detect_contradictions`,
  `resolve_entities`, `link_artifacts` and the gap detector select inputs by vector
  similarity, so an artifact produced against a 384-wide corpus was grounded in a
  different candidate set than a 768-wide one surfaces.

The operator's instruction is to re-synthesise, and the defensible reading of it is:
re-embed everything, re-index everything, and regenerate the retrieval-dependent
stages. Regenerating the per-chunk stages as well costs synthesis time and changes
nothing that was correct — but it is simpler to explain and to verify. Pick one and say
which.

### Synthesis model change

Different mechanics, and nothing implements it today. A new synthesis model does not
invalidate any *vector width* — embeddings are the embedder's output — but it does
invalidate every artifact the old model wrote, because that text is what changes.

So: discard the synthesis-derived artifacts and regenerate them, then re-index the new
ones. `db2_curator_reembed_all`'s `state = 'proposed'` is the wrong verb here; it
preserves text this operation exists to replace.

Note the asymmetry, because it is the thing most likely to get conflated:

| trigger | vectors | artifact text |
| --- | --- | --- |
| embedder change | rebuilt at the new width | regenerate the retrieval-dependent stages |
| synthesis model change | unchanged width, re-indexed after regeneration | **discard and regenerate** |

## §4 Saying what it costs, at the point of choosing

Today the cost is discoverable only by hitting it: the wizard's embedder picker says
nothing about discarding data, and `aimee config set embedder_model` accepted any string
until recently (a typo produced a deployment that searched lexically while reporting
healthy — fixed in #2283).

This has to be stated where the choice is made: the wizard's embedder and synthesis
steps, `aimee config set` for either key when the value differs from the recorded one,
`aimee kb reembed`'s plan output, and the deployment docs. Two losses need
distinguishing, because they are not equally recoverable — **derived vectors** rebuild
from source and lose nothing permanently, while **regenerated artifacts** are new text
that will not match the old.

**This lands before anything that makes the operation easier.** Making a destructive
action more convenient before its consequence is stated is the wrong order.

## §5 Order of work

1. **Warnings (§4).** Standalone, no dependencies, and a prerequisite for the rest.
2. **`Dockerfile.embedder` + publish job.** Mechanical; `Dockerfile.model` and
   `Dockerfile.llm` are the templates.
3. **Embedder mTLS identity + kb client**, mirroring the synthesis hop. #2284 is a
   prerequisite: without it the kb presents no client certificate.
4. **kb image drops torch and the weights**; `EMBEDDER_URL`/sidecar resolution replaces
   the in-process server. Measure the retrieval latency change here (§1) before going
   further.
5. **Drop-and-rebuild for an embedder change** (§3), reusing `db2_reembed.c` and adding
   the regeneration half.
6. **Drop-and-regenerate for a synthesis model change** (§3), which is new work rather
   than a variation on reembed.

Steps 5 and 6 need `kb.reembed_on_dim_change` reconsidered: the documented remedy is
currently gated behind a key in the kb's *own* `aimee.yaml` that `aimee config set`
cannot reach, which makes it the hardest command in the product to run. If these become
supported operations, that gate is in the wrong place.

## §6 The invariant all of this rests on

Everything above assumes DB2 refuses to start when the recorded width does not match the
embedder. That claim lives in a `Dockerfile` comment and in `config.h`. Once switching is
a supported operation, operators meet that guard on purpose rather than by accident, so
it needs to be a tested property and not a comment.

**Untested at the time of writing.** A harness for it exists in the working tree
(`dimdrift.sh`: start the nomic kb image against a 384-wide volume and observe) and has
not been run.
