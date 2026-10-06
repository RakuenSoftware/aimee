# E2B and E4B native adapter validation

Date: 2026-10-06. Host: `.253`.

Two additional model plugins built and passed CPU native-memory validation:
`aimee-gemma4-e2b` and `aimee-gemma4-e4b`. Each model passed 42 lifecycle checks,
with six real inference requests and five native-bank preparations. The stock
model parameters remained on CPU and unchanged. Both fixtures were cleaned up.

The plugins follow the existing aimee-qwen pattern: one dedicated Rust adapter
and library, a checked binding contract, separate command and module namespace,
and the unchanged `aimee-native-runtime==0.3.2` dependency. Rust owns model
geometry, shared K/V source mapping, publication admission and live positions.
The model-specific capture binding handles upstream shared layers without
copying the common serving runtime.

| Gate | Result |
|---|---|
| E2B CPU lifecycle | 42/42 |
| E4B CPU lifecycle | 42/42 |
| Installed plugin identity and checkpoint contracts | 20/20 |
| Shared K/V capture binding | 10/10 |
| Rust adapter tests, including both new model geometries | 6/6 |
| Existing layout and finalization regression tests | 4/4 |
| Final wheel RECORD, namespace, symbols, shared dependency and metadata audit | Passed |

Lifecycle checks include cold and warm recall of a random six-digit canary,
warm bank reuse, conditional correction and a changed bank, retirement and
abstention, restored memory, changed-recipient rejection, and real server
revocation before another decode. Aimee supplied the current source through a
standard enrolled mTLS client with a pinned server certificate. The canary was
absent from the main conversation. The main conversation cache remained unchanged
after each private decode. Native banks were published and admitted through the
installed Rust adapter, then read by the established Transformers CPU attention
harness. Recipient-change rejection in this run is enforced by that harness;
it is not a separate qualification of the vLLM serving broker.

The application image was
`ghcr.io/rakuensoftware/aimee@sha256:e42752b9aafa9703ce9f11a703503a8bf0ea4fbe58dd6abbc8ccab5699904f0b`,
from source `73cd98ce5b350323dd8956bb750d45a500ead3db`. The current `:testing`
manifest was checked after execution and still resolved to that digest.

E2B used the existing verified CPU checkpoint at revision
`3e22461f65e89153144f8adb70e3b8c2cc9845a7` of `google/gemma-4-E2B-it`.
E4B used revision `ee0ef6023621cff504d758262d4e04895a5af4a2` of
`google/gemma-4-E4B-it`; its 15,992,595,884-byte safetensors file matched the
upstream SHA-256 `cfbd3d2f1cd71bd471c37fe2bf8546d5028d41e5736f64e1ca6c6b8893125503`.

The first E2B attempt lacked the harness dependency `accelerate`. A later
attempt found an assertion bug: the absence response `unknown` was already
allowed in the prompt and must not be treated as leaked memory. Cleanup also
needed to accept HTTP 404 for an already retired fixture. These harness defects
were corrected before the full passing runs. Failed attempts remain archived.

This validates CPU model and adapter behavior. The vLLM serving backend, GPU
execution, and public signed release installation remain unqualified by these
runs. The final wheels are unsigned local validation candidates. Their Rust
libraries are byte-identical to the libraries used during inference; the final
shared-K/V binding passed its structural tests. Finalization added dependency
metadata and notices while preserving executable members.

| Plugin | Final wheel SHA-256 |
|---|---|
| gemma4-e2b | `aab9e90748151e6d700aaad1d60585bef055a35fab5b1c2b73098f0abdb4f4c0` |
| gemma4-e4b | `be890245b08e05cd2878dce3686e095699d5064a3f31d4f895745e3c9eb461d2` |

Raw named-verdict reports are alongside this file. Model weights, credentials,
and private source remain outside the public application repository.

## Release qualification in progress

The two wheel files were downloaded independently from PR #3013 and matched
both SHA-256 values above. E2B passed a fresh isolated environment installation,
dependency checks, console startup and serving-module imports. E4B's equivalent
installation is still pending.

Actual vLLM CUDA lifecycle validation has started on the RTX 5080 using the
E2B Q8_0 checkpoint, verified against its pinned upstream SHA-256. Client
enrollment succeeded after correcting the test harness to retry the standard
CLI enrollment. The serving process started, but no completed inference or
lifecycle verdict has been observed. The host subsequently stopped responding
to SSH while remaining reachable by ICMP. A stop request for the owned model
unit could not be confirmed. Host memory and I/O pressure are unconfirmed;
there is no evidence yet establishing the cause.

The first E4B Q8_0 download was truncated and rejected by its checksum check.
Those bytes were quarantined; a new download is pending verification. No
rejected checkpoint was loaded. Both candidates remain unsigned and are not
release-qualified. The 12B, 26B and Qwen 3.8 27B smoke tests have not started.
