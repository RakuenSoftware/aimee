# Search egress policy: separate untrusted destinations from operator-configured endpoints

*Filed as a precondition record for
[surface-neutral-retrieval-substrate.md](../done/surface-neutral-retrieval-substrate.md)
("S2"). Classification: **security, medium**.*

> **PROMOTED TO BLOCKER.** This was filed as a latent structural gap, on the
> reasoning that `web_search`'s endpoints are compile-time constants so nothing
> attacker-controlled reaches them today. That holds only while search returns
> snippets.
>
> The capability assessment
> ([web-retrieval-capability-map.md](../done/web-retrieval-capability-map.md)) ranks
> "fetch and extract from the top N search results" as the highest-value change
> available. Those result URLs come from a third-party engine and are influenced
> by whoever ranks in it. Fetching them through a path with no egress validation
> is a direct SSRF, not a latent one.
>
> This must land before search fetches anything.

## Problem

`src/posix/web_read.c` implements a correct egress posture for untrusted
destinations: resolve the host once, validate the resolved address against a
private/reserved deny-list (`web_egress_addr_blocked`,
`egress_resolve_validate`), pin the connection to that validated address so a
rebinding resolver cannot swap in a private address between check and connect,
and refuse to follow redirects.

`src/server/web_search.c` has none of it. It reaches the network via
`agent_http_get` (`:268`, `:302`) and `http_retry_post` (`:375`). A grep for
`web_egress_addr_blocked` / `egress_resolve_validate` finds callers only in
`web_read.c` and `src/tests/test_agent.c`.

## Why this is not simply "share the validator"

The obvious fix — call `web_egress_addr_blocked` before `agent_http_get` — is
wrong in two distinct ways.

**These are two policies, not one.** A model-supplied page URL is an untrusted
destination and should be denied private/reserved addresses. A SearXNG endpoint
is operator-configured and may legitimately be `127.0.0.1`, a private LAN
address, or a cluster service address. Applying the page-reader deny-list to the
configured backend would break legitimate self-hosted deployments — which is the
documented reason the SearXNG backend exists.

**Validation without pinning leaves a TOCTOU gap.** Validating a resolved
address and then handing the *hostname* to `agent_http_get`, which resolves
again, restores exactly the rebinding window `web_read.c` was written to close.
Reusing the validator alone is not reusing the defence.

## Current exposure

Limited but not zero. The DuckDuckGo and Tavily endpoints are compile-time
constants (`https://html.duckduckgo.com/html/?q=`,
`https://api.tavily.com/search`), so they are not attacker-steerable. The
SearXNG endpoint is configuration-supplied, so the reachable destination is
operator-controlled rather than fixed. The gap is a latent structural one:
today's safety comes from the endpoints happening to be constants, not from a
control.

## Direction

1. Introduce two named policies rather than one predicate — roughly
   `EGRESS_UNTRUSTED_DESTINATION` (deny private/reserved; pin; no redirects) and
   `EGRESS_CONFIGURED_SERVICE` (private addresses permitted only when the
   operator has explicitly authorized that endpoint in configuration).
2. Reuse the *transport* behaviour, not just the validator: resolve once,
   validate, connect to the validated address, apply an explicit redirect policy.
   This likely means a shared egress-aware transport helper rather than a bare
   check before `agent_http_get`.
3. Require explicit operator authorization for a configured endpoint that
   resolves to a private or reserved address, so the permissive path is opt-in
   and visible rather than implicit.

## Acceptance

- A model-supplied URL resolving to a private or reserved address is refused.
- A configured SearXNG endpoint on a private address works only with explicit
  authorization, and is refused without it.
- A rebinding resolver cannot change the connected address after validation, on
  both the page-read and search paths.
- Redirect behaviour is explicit and tested on both paths.
