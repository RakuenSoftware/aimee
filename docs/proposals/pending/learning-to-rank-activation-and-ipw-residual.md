# Learning-to-rank activation and IPW residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`learning-to-rank-from-interactions.md`](../done/learning-to-rank-from-interactions.md).

## Delivered foundation

The serving feature rows, retrieval event/outcome stores, pointwise and pairwise fitter, weight
consumption, periodic offline fit, and fail-closed benchmark promotion gate are shipping.

## Remaining work

- Decide and safely activate `learning_implicit_retrieval_outcome`, which currently defaults off.
- Capture the complete bounded candidate pool or a deliberately sampled pool rather than only the
  first eight results.
- Record the serving policy's selection propensity for every labelled candidate and derive capped
  inverse-propensity weights from that observed value; do not synthesize weights after the fact.
- Supply a real time-split held-out benchmark and segment gates before enabling promotion.

## Acceptance

Live impressions contain candidate scores/ranks, outcomes, sampling decision, and non-default
propensity. Replay reproduces the serving policy, a known-worse policy scores worse under capped IPW,
the held-out gate has adequate power, and activation has an explicit rollback switch and volume cap.

