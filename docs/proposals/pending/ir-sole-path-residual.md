# IR sole path: implementation and qualification residual

- **State:** PENDING. The supported browser/Linux cutovers are implemented;
  live testing still exposes consumer repairs, including canonical output-limit
  retry classification. Final-source acceptance and production promotion remain
  pending. See the current handoff for completed cases and retained failures.

**Archived parent:** [`ir-sole-path-and-pluggable-stages.md`](../done/ir-sole-path-and-pluggable-stages.md)

## Implemented continuation

Supported provider ingress, response parsing and streaming use canonical IR and
registered stages. Canonical streaming is enabled; no default-off relay switch
remains. Paths requiring policy/completion repair buffer before canonical delivery.
The retired `raw_responses`, `anthropic_stream_feed_openai`,
`build_provider_body` and `translate_request` bypasses are removed. Native-format
handling remains at explicit translation boundaries. Mutation-aware replay
preserves untouched native request/cache data and rebuilds edited requests.

The IR bypass checker prevents reintroduction of retired paths and guards the
migrated consumers; its exception ledger is empty. Canonical primary/delegate
history, model switching, browser controls, source-bound native replay and honest
legacy-import fidelity are implemented. Pinned SDK/provider fixtures and actual
store/kernel journeys establish the recorded supported-path behavior; they do
not establish live upstream vendor acceptance.

## Remaining qualification and promotion

- Qualify the final integrated commit with the local regression gates, fresh CI
  and every configured deployment tier. Earlier green commits do not qualify
  subsequent changes.
- Retain the explicit distinction between pinned fixture coverage and live
  upstream/subscription acceptance, which remains separately unqualified.
- Obtain the recorded independent approving identity and live-control evidence
  required by `OWNERS.md` before production promotion; then verify actual KB-free
  capture and reuse on `.100` through the supported deployment path.

macOS/Windows thin-client builds do not establish native server execution on
those platforms. Unsupported native/credential profiles remain explicitly
refused. The [current handoff](../../IR_MIGRATION_HANDOFF.md) records qualification,
platform limits and promotion obligations. This proposal does not claim a
completed production rollout.
