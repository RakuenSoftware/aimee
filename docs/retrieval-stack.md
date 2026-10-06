# Retrieval stack

Retrieval runs in the instance that owns the selected data. Server serves personal memory and private
code; KB serves its shared corpus. They use the same Go memory implementation with different tables,
scopes, and capabilities. See [Server and KB](SERVER_AND_KB.md).

## Current memory path

```text
selected store + verified caller + scope
  -> one owner transaction
  -> eligible lexical / versioned semantic / graph candidates
  -> rank fusion and bounded selection
  -> evidence and source versions
  -> budgeted context or served view
  -> source revalidation at the consuming boundary
```

The Go [memory owner](modules/memory.md) implements memory retrieval. Remaining native adapters
handle transport and materialization. KB document/code retrieval has additional corpus-specific
paths; a memory ranking guarantee should not be generalized to every search endpoint.

## Eligibility before ranking

The current shared-memory predicate, `current-validity-v18`, checks active lifecycle, suppression,
world-valid time, configured utility horizon, and derived-input currency. It uses the transaction's
clock. Scope, row security, embedding fingerprints, and source-evidence visibility add further gates.
See [eligibility.go](../server-go/modules/memory/eligibility.go).

Current reads exclude inactive and expired rows regardless of legacy lifecycle flags. Exact-ID
history inspection uses a separate contract. Assertion search supports world-valid and belief-time
queries; a legacy `get --as-of` response is not a reconstruction of belief time. Personal history
selects an explicitly retained revision by owner/version identity.

Ranking cannot turn an ineligible record into evidence. Unknown/malformed time values and required
SQL failures remain errors rather than successful partial retrieval. Model unavailability is handled
separately: an optional semantic lane may be unavailable while lexical retrieval still succeeds.

## Embedding

Standard [Server Compose](../compose.yaml) and [KB Compose](../compose.kb.yaml) deploy embedding as
a separate sidecar. Each composition owns its model configuration and TLS identity. The default
model is `bekko-a25m`, with dimension `384`; synthesis uses an optional profile. External endpoints
are configurable. The standard deployment does not require an embedded model inside the application
container or a KB for personal semantic recall.

A configured external endpoint receives the text to embed. Personal vectors stay in Server's store;
shared vectors stay in KB's store. Model names, dimensions, pooling, and query/document prefixes all
matter to the vector space. A same-width replacement can still invalidate the corpus.

### Memory generations

Go memory stores versioned embedding generations, including model identity, dimensions, source
revision, and input fingerprint. Shared whole-record and derived-unit lanes require a pinned active
version. They discard stale parent/unit vectors and invalid dimensions before ranking. One query
embedding is reused across the qualifying shared semantic channels.

Memory's generation columns are unconstrained `vector` columns with dimensions tracked per version.
A KB fixed-width dimension reset does not own or drop these generations. Rebuild and cutover stay
with Go memory. See [memory module](modules/memory.md) and
[shared semantic retrieval](../server-go/modules/memory/shared_recall.go).

### KB document and code vectors

KB document/code schemas also have dimension and serving-identity checks. Their fixed-width reset
has a different contract from memory generation cutover. Review the affected vector family before
using a repair command. [Change the KB embedder](runbooks/change-embedder.md) covers that operational
path; record source counts, model identity, dimensions, and recall canaries before changing a corpus.

The [frozen embedder selection report](validation/embedder-selection-frozen-ab-v1.md) records an
older measured comparison. It explains the selection under its own corpus and configuration; it is
not a fresh benchmark of the current Go owner.

## Fusion

Lexical matching covers names, identifiers, and text. Versioned dense retrieval adds semantic-only
candidates. Graph expansion adds bounded relationships whose source records remain visible and
eligible. Reciprocal-rank fusion combines ranked lists without treating raw scores as comparable.
Repeated IDs within one arm get one vote; agreement across distinct arms can contribute separately.

Shared scope ordering favors project, then workspace, then global evidence. Personal retrieval
remains user-scoped. The owner deduplicates and bounds the result. Exact constants and channel
behavior live in [fusion.go](../server-go/modules/memory/fusion.go) and the
[module contract](modules/memory.md), which also describes semantic unit floors and PageRank.

## Sub-query fusion

The older C ranker used heuristic and LLM query decomposition with an interleaved candidate merge.
That implementation has been retired. It must not be described as the current default Go retrieval
path or configured through its old benchmark-only environment flag.

The [compatibility decisions](proposals/pending/memory-reliability-retrieval-compatibility.md)
record retired query expansion and ranking behavior. Current evaluation must use the Go owner and
its policy fingerprint. A historical result from the C candidate pool is not a baseline for a
changed Go policy without a controlled comparison.

## Reranking and optional policies

The current stack does not add a cross-encoder reranker. The
[historical retrieval report](validation/retrieval-stack-report-2026-07-30.md) explains its removal
under the measured configurations. Go still performs ranking, gating, and optional graph scoring;
removing a cross-encoder does not remove those stages.

PageRank is opt-in. Utility-horizon and selection policies also retain their own disabled/default
modes; shipping an implementation does not activate it. See [Memory](MEMORY.md) and
[release preparation](validation/release-0.4.6-preparation-2026-09-27.md). Do not claim quality gains
from a policy without paired evaluation on the same corpus and model identity.

## Evidence and delivery

Recall and served views preserve source identities and versions. The owner budgets serialized
context, while source revalidation protects later materialization and provider dispatch against
changed or erased inputs. A successful search is not proof that the same bytes were eventually
sent to a model. Context composition across personal and shared stores is not one atomic snapshot.

`memory serve` and claim cards expose bounded views with receipts and diagnostics. Optional synthesis
uses the selected instance's configured endpoint and should retain evidence attribution. Support and
abstention checks do not equate graph popularity or a high similarity score with corroboration.

## Configuration and checks

Use the [generated configuration](gen/configuration.md) and [command reference](gen/cli-commands.md)
for supported settings and operations. A listed legacy key still needs a live consumer before it can
be treated as an effective policy control.

For a retrieval change, test each supported placement and public route. Pair an expected visible
record with excluded foreign-scope, archived, suppressed, expired, stale-vector, and revoked-source
records. Exercise an unavailable model and a failed required SQL read separately. Verify the final
context and receipts, not just the candidate list.

Evaluation reports must bind corpus hash, schema, effective policy, embedding identity, dimensions,
case IDs, and latency scope. The isolated Go evaluator measures the owner; its latency does not
include every client/Server/KB hop. See [Memory evaluation](MEMORY.md#isolated-go-evaluation-transport)
and [Benchmarks](BENCHMARKS.md).
