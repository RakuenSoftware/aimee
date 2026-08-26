# IR sole path: response and legacy-path residual

- **State:** PENDING. Residual scope only.

**Archived parent:** [`ir-sole-path-and-pluggable-stages.md`](../done/ir-sole-path-and-pluggable-stages.md)

## Shipped baseline

The request stage registry, response registry, orchestration seam, governance response stage, and delegate/workflow hooks are present and tested.

## Remaining deliverables

- Route every provider response through the response IR and registered stages.
- Replace `raw_responses` and `anthropic_stream_feed_openai` compatibility paths with explicit IR adapters.
- Retire direct `build_provider_body` / `translate_request` construction once parity is proven.
- Promote the relay from default-off only after streaming, tool-call, error, and cancellation parity gates pass.
- Add live-provider equivalence tests and a checker preventing new bypasses.
