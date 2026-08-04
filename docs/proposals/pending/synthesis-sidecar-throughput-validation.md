# Synthesis sidecar throughput validation

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`synthesis-sidecar-and-embedder-axis.md`](../done/synthesis-sidecar-and-embedder-axis.md).

## Delivered foundation

Synthesis is an independently published, baked-model, mTLS-protected sidecar. The managed wizard and
compose path deploy it without rebuilding or recreating the data-bearing KB image.

## Remaining work

Measure the cost of the container-network plus mTLS hop against the former in-container loopback path
for E2B and E4B after model warm-up. Separate handshake, transport, prompt evaluation, and generation
time, and report steady-state throughput rather than wall-clock startup.

## Acceptance

A reproducible benchmark records hardware, images/digests, prompt corpus, concurrency, warm-up,
latency percentiles, and tokens/second for both paths. The result states an explicit acceptable
regression budget or creates a focused performance follow-up when it is exceeded.

