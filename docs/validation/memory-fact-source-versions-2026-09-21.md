# Plain-text fact source versions and assembly evidence

The legacy facts channel returned only text, losing the assertion identities
needed for later source checks. The Go owner now returns a versioned fact
projection beside the unchanged text. It binds exact owner/assertion/revision
identities and direct memory-parent revisions to the rendered bytes and ordered
selection. Each assertion and its parents are observed in the same SQL statement.
The existing multi-entity traversal is not claimed as one collection snapshot.

Metadata is collected in the existing fact query, without a per-fact RPC or
additional database query. Selected sources use the existing bounded primary-key
parent probes. Sensitive, low-confidence, oversized and capacity-omitted facts
contribute no retained source references. Parent overflow refuses the projection;
ordinary legacy recall keeps its existing contract. Projection construction hashes
the selected metadata once; consumers independently verify the commitment.

Go ingress assembly checks the projection before admitting the block. If packing
omits the facts, its final projection carries no retained items or source-version
claim. If retained, exact IDs and selection identity survive unchanged. The C host
only forwards Go-issued references to its existing retrieval-event writer after
integrity acceptance. Failed assembly or integrity rejection emits no fact evidence.
The reviewed external adapter remains host transport; the C bus is unchanged.
Older text-only replies retain no source-evidence claim.

Validation:

- The full Go/PostgreSQL memory suite passes. Restricted runtime-role replay checks
  selected assertion/parent identities, public command transport, a changed parent
  with identical fact text, literal capacity omission, and bounded parent overflow.
- Targeted runtime-role and assembly race tests pass. Projection tests cover exact
  IDs above 2^53, whole-block omission, text/revision/parent/channel/ID mismatch,
  duplicate references, null metadata and incorrect byte accounting.
- The real C-host/Go-process ingress test checks retained fact references after
  packing and integrity acceptance, plus zero evidence for omitted/rejected blocks.
- Native build/routing, standalone export, all 17 S1 tests and documentation/link
  checks pass. The full lint run passed 76 gates; the ownership gate then passed
  after reviewing and updating hashes for the external host and its transport test.
  No memory policy moved into C. Four fresh authenticated KB checks
  were added for exact source binding, byte/selection commitments, stable repeated
  reads and changed-parent binding with identical text. Fresh deployment is pending.

This closes the plain-text fact assembly-evidence gap. It does not implement
release-time owner revalidation or durable provider preparation/dispatch receipts.
Transitive lineage, collection dependencies, restore-resistant owner incarnation,
remaining channels and final provider binding remain open. No proposal is certified
complete by these observed source versions.
