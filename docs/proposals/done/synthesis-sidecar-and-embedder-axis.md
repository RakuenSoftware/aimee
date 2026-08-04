# Proposal: synthesis as an mTLS sidecar, and a kb image matrix on one axis

- **State:** DONE — sidecar topology delivered and archived 2026-08-04; residual measurement extracted.

> **Archived after partial delivery.** The independent `aimee-llm-e2b/e4b` images, input-keyed
> publish guards, stunnel mTLS hop, KB-issued identities, managed-compose wiring, and wizard model
> selection are shipping. The only explicitly unmeasured premise is now
> [`synthesis-sidecar-throughput-validation.md`](../pending/synthesis-sidecar-throughput-validation.md).

Originally a design-only amendment to the image
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

## The hop is mTLS, like every other container hop

mTLS is the standard for container-to-container communication in this project.
`server → kb` already works this way: `kb_mtls_start()` binds a listener whose
context is built from the KB's own CA. A new inter-container hop follows the same
standard, and needs no argument from a threat model to justify it.

That settles *what*. The only design content is *which component terminates it*, and
llama-server is not a candidate — the pinned `b10218` binary offers `--api-key` and
`--ssl-cert-file`/`--ssl-key-file`, with no client-cert or CA verification flag. That
is unremarkable; terminating mTLS is not an inference server's job. It means the
sidecar looks like every other mTLS endpoint here: the terminator owns the
network-facing listener, and the thing it protects sits behind it.

**Terminator: `stunnel` from the distro, not new code.** A hand-rolled TLS listener
would take on the burden this repo already names for llama.cpp — "THIS MAKES US THE
VENDOR... `LLAMACPP_VERSION` is now the only thing deciding which llama.cpp a user
runs, and it does not move on its own" — for a component whose failure mode is
silent exposure. `stunnel` from apt inherits Debian's CVE fixes instead.

llama-server binds `127.0.0.1` inside the sidecar. That is not defence in depth so
much as arithmetic: loopback in the container's own network namespace is unreachable
from any other container, so stunnel is the only path in. `--api-key` costs nothing
and is worth setting, but it is not what makes the endpoint safe.

**The caller needs client-cert support it does not have.** `scripts/llm-chat.py`
calls `urllib.request.urlopen` with no TLS context. It already handles
`SYNTHESIS_API_KEY` as a bearer, including a `cmd:` form that shells out for the
value — but presenting a certificate needs an `ssl.SSLContext` with
`load_cert_chain`, plus the identity paths as table-declared settings, per #2242's
one-name-per-setting rule. This is the real work on the client side.

**PKI ownership.** The KB already runs a CA: `kb_mtls_start()` calls
`kb_pki_ca_load_or_create_custodied($data_dir/kb-ca)` and issues its own server cert
from it. For this hop the KB is the *client*, so the sidecar must verify against the
KB's CA — which means the KB issues the sidecar's server identity.

### Bootstrap ordering

The deployment sequence is:

1. deploy `aimee-server`
2. run the wizard
3. deploy and connect `aimee-kb`
4. deploy and connect `aimee-llm` — **only if synthesis was selected in the wizard**

This removes the problem rather than solving it. The sidecar is deployed *after* the
KB is up and connected, so the CA already exists at the moment the identity is
needed. There is no blocking wait, no files-appear race, and no need to widen
`aimee-authority-bootstrap`'s remit: the ordering is a property of the wizard flow,
not something compose has to be coerced into.

Step 4 being conditional also matches the existing shape — the retired `aimee-llm`
service was gated behind `profiles: ["llm"]`, and `COMPOSE_PROFILES` already selects
the kb this way.

What step 4 must do, and what does not exist yet: the KB mints a server cert for the
sidecar's DNS identity (`aimee-llm`) plus a client cert for itself, and the deploy
layer places that material where both containers can read their own half. The KB has
`kb_pki_issue_server_cert()` for its *own* listener; issuing on behalf of a third
party is new surface.

### What this simplifies in the wizard

`frontend/src/setup/deployTopology.ts` currently derives the synthesis choice from
the kb tag — "decided by the tag you pulled (`aimee-kb-llm-e2b` vs `-e4b`)" — which
is why #2242 had to disable the local-model options on an image that bakes none.

Once synthesis is its own container that coupling goes away: any kb image can have
synthesis added or removed afterwards, because the two are no longer the same
artefact. `bundledSynthesisModel(cfg)` and the option-disabling logic that depends on
it can go, and "off" stops being a property of the image you happened to pull. The
three-way surface the wizard already presents — off, local, external — survives
unchanged; only what "local" *means* changes, from a tag constraint to a service.

### Synthesis becomes an in-place upgrade

This is the capability the split unlocks, and it is worth more than the build-time
saving that motivated it.

Today, moving between E2B and E4B — or adding local synthesis to a running
deployment — means pulling a different kb tag and recreating **the container that
holds the database**. A multi-gigabyte swap of a data-bearing service, to change a
model that has nothing to do with the data.

As a sidecar it is a stateless container swap: pull `aimee-llm-e4b`, recreate one
service, leave the kb running. Add synthesis to a deployment that never had it,
downgrade to E2B on a box that turned out too small, or remove it and point
`SYNTHESIS_ENDPOINT` at an external provider — none of it touches the kb or its
volume. Both surfaces already exist to drive it: the wizard's Deploy step, and
`/v1/deploy/apply` behind it.

The asymmetry with the embedder is now the honest one, and worth stating plainly in
the operator docs: **the embedder is a one-way door and synthesis is not.** DB2
records the vector-column width and refuses to start on drift, so an embedder change
means re-embedding the corpus. `docs/UPGRADING.md` already says the synthesis axis is
not one-way; under this design that stops being a technicality about tags and becomes
an operation someone can actually perform.

A is recommended: the CA is already the KB's, and a sidecar that waits for its
identity is a smaller change than a new provisioning path. B is the fallback if the
ordering proves awkward under the managed-compose deploy path, which recreates
services independently.

## Slicing

Dependency order, each independently reviewable:

1. **`aimee-llm` image + publish workflow** — llama.cpp, baked model, stunnel,
   entrypoint, the two-way rebuild guard. No kb changes; nothing consumes it yet.
2. **PKI provisioning** — the KB issues the sidecar's server identity and its own
   client cert at step 4 of the deployment sequence, plus the identity settings. The
   ordering is settled; how the material is transported is what this step designs.
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
- llama-server's flag surface, from `--help` on the pinned binary.
- The deployment ordering, which is settled rather than inferred: server, wizard, kb,
  then the sidecar only if synthesis was selected.
- The `aimee-model-*` images already exist at `UD-Q6_K_XL`, so the weight layer is
  shared rather than refetched.

## What is not

- No `aimee-llm` image has been built. stunnel's client-cert verification against
  `kb_pki`-issued material is unexercised.
- The no-embedder kb variant is asserted to be expressible by skipping the torch and
  weight stages; the conditional-stage idiom exists in the Dockerfile, but this
  specific variant has not been built.
- Issuing a certificate for a third party is new surface. `kb_pki_issue_server_cert()`
  exists but is used for the KB's own listener; how the sidecar's identity is
  requested, and how the deploy layer places each half where its container can read
  it, is undesigned.
- Whether the wizard's "deploy and connect" step can carry that material through
  `deploy_apply.c` without a new mechanism is unknown. The ordering is settled; the
  transport for the identity is not.
- No measurement of synthesis throughput over the hop versus loopback. The extra
  cost is a TLS handshake per connection against a model that takes seconds per
  request, so it is expected to be noise — expected, not measured.
