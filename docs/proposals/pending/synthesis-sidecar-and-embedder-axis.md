# Proposal: synthesis as an mTLS sidecar, and a kb image matrix on one axis

- **State:** PENDING — design only, no implementation in this PR. Amends the image
  topology landed in **PR #2242** (six `aimee-kb` tags, weights baked into each) and
  the channel wiring in **PR #2253**, whose `-llm` legs **PR #2255** removes as the
  first step of this design.
- **Author:** JBailes
- **Date:** 2026-08-02

## Thesis

#2242 baked llama.cpp and the synthesis weights into the `aimee-kb` image. The
reason was sound and is not in dispute: a first-run download fails on a rate limit,
a proxy, a flaky link or an air-gapped host, and reaches the operator as "synthesis
never started" long after the deploy looked fine. An image either has its model or
it does not.

What it got wrong is *which* image carries them. Baking synthesis into the kb image
couples a multi-gigabyte, near-static artefact to the most frequently rebuilt one:

- `aimee-kb`, `aimee-kb-llm-e2b` and `aimee-kb-llm-e4b` are each a full kb build,
  under **separate `cache-from` scopes**. One kb code change rebuilt the LTO C
  binary, postgres, pgvectorscale, torch and the embedder weights **three times**,
  per channel, and pushed ~10 GB twice.
- The inputs that actually determine a synthesis image — the model, the quant, the
  pinned llama.cpp — change on the order of never.

Splitting them keeps the baked-weights property and removes the coupling.

## Target topology

**`aimee-kb`, on the embedder axis only:**

| tag | embedder | notes |
| --- | --- | --- |
| `aimee-kb` | none | external `EMBEDDER_URL` only; **skips torch and the weight download entirely** |
| `aimee-kb-a25m` | bekko-a25m, 384 | |
| `aimee-kb-nomic` | nomic-v2, 768 | |

The no-embedder variant is new and is not merely a smaller matrix entry: today the
Dockerfile always installs CPU torch and bakes embedder weights, so a deployment
using an external embedder still carries both. That variant cannot currently be
expressed.

**`aimee-llm`, on the model axis only:** `aimee-llm-e2b`, `aimee-llm-e4b`. Carries
llama.cpp plus one baked GGUF. Deployed beside `aimee-kb`;
`SYNTHESIS_ENDPOINT` names it over the compose network rather than loopback.

Five images replace six, and the two multi-gigabyte ones leave the kb rebuild path.

## Rebuild discipline

`aimee-llm` must not rebuild because kb code changed. Enforced **twice**, because a
path filter alone is one careless edit away from being wrong:

1. A path-filtered trigger: `Dockerfile.llm`, `deploy/container/aimee-llm-entrypoint.sh`,
   `scripts/fetch-synthesis-model.sh`, the model list, the pinned `LLAMACPP_VERSION`,
   and the workflow itself.
2. A skip-if-already-published check keyed on the inputs — model, quant, llama.cpp
   version — the same pattern the existing `models` job uses. Even a broad or
   accidental trigger then costs one `docker manifest inspect`.

Point 2 is what makes the guarantee robust rather than aspirational.

## The hop must be mTLS

Moving llama-server off loopback is a security regression unless the hop is
authenticated. The first boot of `aimee-kb-llm-e2b:testing` logged:

```
llama_server: CORS is set to allow all origins ('*') and no API key is set
llama_server: this can be a security risk (cross-origin attacks)
```

Contained inside the kb container. An unauthenticated inference endpoint the moment
it moves onto a shared network — which would cut directly against #2248 (refuse
cleartext bearers) and #2250 (close the unauthenticated plain listener).

**llama-server cannot do mTLS.** Verified against the pinned `b10218` binary:

| flag | present |
| --- | --- |
| `--api-key`, `--api-key-file` | yes |
| `--ssl-key-file`, `--ssl-cert-file` | yes — server-side TLS |
| client-cert / CA verification | **none** |

So mTLS requires a terminator in front of it. Three consequences:

**1. Terminator: `stunnel` from the distro, not new code.** TLS termination with
client-cert verification is security-critical, and this repo already notes the cost
of vendoring llama.cpp — "THIS MAKES US THE VENDOR... `LLAMACPP_VERSION` is now the
only thing deciding which llama.cpp a user runs, and it does not move on its own".
A hand-rolled TLS listener takes on that same burden for a component where the
failure mode is silent exposure. `stunnel` from apt inherits Debian's CVE fixes.
llama-server stays bound to `127.0.0.1` with `--api-key` as defence in depth, so it
is never reachable directly even if the terminator is misconfigured.

**2. The caller needs client-cert support it does not have.** `scripts/llm-chat.py`
calls `urllib.request.urlopen` with no TLS context. It already handles
`SYNTHESIS_API_KEY` as a bearer, including a `cmd:` form that shells out for the
value — but presenting a certificate needs an `ssl.SSLContext` with
`load_cert_chain`, plus the identity paths as table-declared settings, per #2242's
one-name-per-setting rule.

**3. PKI ownership, and the one real open question.** The KB already runs a CA:
`kb_mtls_start()` calls `kb_pki_ca_load_or_create_custodied($data_dir/kb-ca)` and
issues its own server cert from it. For this hop the KB is the *client*, so the
sidecar must verify against the KB's CA — which means the KB has to issue the
sidecar's server cert and publish the CA to it. That is a bootstrap ordering
dependency the current stack does not have.

### Bootstrap options

| option | how | cost |
| --- | --- | --- |
| **A. KB provisions, sidecar waits** (recommended) | KB issues the sidecar's server cert + its own client cert into a shared volume; sidecar blocks on the files with an explicit log line | one ordering dependency, no new component; matches where the CA already lives |
| B. `aimee-authority-bootstrap` one-shot | extend the existing one-shot provisioner to mint the pair before either starts | reuses a component built for exactly this; widens its remit |
| C. Sidecar owns its own CA | KB enrols against it | inverts the existing trust direction for no gain |

A is recommended: the CA is already the KB's, and a sidecar that waits for its
identity is a smaller change than a new provisioning path. B is the fallback if the
ordering proves awkward under the managed-compose deploy path, which recreates
services independently.

## Slicing

Dependency order, each independently reviewable:

1. **`aimee-llm` image + publish workflow** — llama.cpp, baked model, stunnel,
   entrypoint, the two-way rebuild guard. No kb changes; nothing consumes it yet.
2. **PKI provisioning** — whichever bootstrap option survives review, plus the
   identity settings.
3. **Client-side mTLS** — `SSLContext`/`load_cert_chain` in the sidecar clients.
4. **Compose wiring** — the service, and `SYNTHESIS_ENDPOINT` pointed at it.
5. **kb retopology** — drop llama.cpp and the model stages; embedder axis becomes
   none/a25m/nomic; publish matrices and docs follow.

Synthesis keeps working throughout: until step 4 the kb resolves whatever
`SYNTHESIS_ENDPOINT` it is given, exactly as it does for an external endpoint today.

## What is already verified

- **Bundled synthesis answers requests.** `aimee-kb-llm-e4b:testing` on a test host
  scored 10/10 on `scripts/aimee-kb-docker-smoke.sh`, including
  `entrypoint started llama-server on :8761` and **`bundled synthesis completed a
  request`**. `aimee-kb-llm-e2b:testing` booted the same way. Model load was ~2.6 s
  (E4B) and ~2.2 s (E2B).

  This is the load-bearing evidence for the whole proposal: the llama-server
  invocation, the baked GGUF, the `MODEL_ID` plumbing and the kb's own synthesis
  client path are all proven on real hardware. Moving to a sidecar changes the
  address and adds mTLS; it does not change the mechanism underneath.
- `aimee-kb:testing` smoke: 8/8, including a live 384-dim bekko embed round-trip
  with no vector-space refusal.
- Both smokes ran against the *published* `:testing` images, pulled from ghcr, not
  local builds.
- llama-server's flag surface, from `--help` on the pinned binary (table above).
- The `aimee-model-*` images already exist at `UD-Q6_K_XL`, so the weight layer is
  shared rather than refetched.

## What is not

- No `aimee-llm` image has been built. stunnel's client-cert verification against
  `kb_pki`-issued material is unexercised.
- The no-embedder kb variant is asserted to be expressible by skipping the torch and
  weight stages; the conditional-stage idiom exists in the Dockerfile, but this
  specific variant has not been built.
- Whether the managed-compose deploy path (`deploy_apply.c` re-running compose)
  preserves the ordering option A needs is **unknown** and is the first thing step 2
  must establish.
- No measurement of synthesis throughput over the hop versus loopback. The extra
  cost is a TLS handshake per connection against a model that takes seconds per
  request, so it is expected to be noise — expected, not measured.
