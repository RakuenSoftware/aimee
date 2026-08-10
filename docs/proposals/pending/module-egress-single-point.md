# Proposal: One egress module, so unlogged external communication becomes impossible

- **State:** PENDING — architecture proposal; no implementation has started.
- **Date:** 2026-08-10.
- **Charter roles:** Enforce / Constrain-Verify.
- **Thesis:** modules are forbidden from talking to the outside world. Communication
  initiated from outside arrives over the event bus; communication a module initiates
  leaves through one egress module, also over the bus. Direction selects the door; it
  never grants a module the right to open a connection itself. The end state is that
  modules have no network at all, so "all egress is audited" stops being a rule people
  follow and becomes a property of the system.

## 1. Problem

The module rule today is that a module may initiate outbound communication — the delegate
module must reach an LLM, the git module must reach its forge — provided the call is
logged to the event bus.

That rule cannot hold itself up. A module that dials out and omits its bus event works
perfectly: the request succeeds, the feature behaves, nothing fails. The only casualty is
that the call is invisible to governance, and nothing anywhere notices. The failure is
silent, and silent failures accumulate.

Reviewing for it means auditing intent — "is this call accompanied by its event?" — on
every call site, in every module, forever. That is the kind of invariant that is correct
on the day it is written and decays quietly from then on.

There is a second cost. Because the module that dials is the module that authenticates,
every module that talks outward must hold a credential. Provider tokens, forge tokens, and
vault material spread to each caller, and the blast radius of any one module grows to
include the secrets it needs to do its job.

## 2. Proposal

One `egress` module owns all internally-initiated external communication.

- A module that wants to reach outside sends a request over the bus to `egress`.
- `egress` authorizes the call, records it, performs it, and returns the result.
- No other module opens a connection.

Combined with the existing inbound rule, every crossing of the process boundary has
exactly one path:

| Direction | Door |
| --- | --- |
| Externally initiated (a browser, a tool, a peer) | event bus, via C |
| Internally initiated (LLM, forge, provider API) | `egress` module, via the bus |

The invariant this buys is not "we log all egress" but **egress cannot happen unlogged**,
because the audit record is the call path rather than something attached to it.

### Enforcement

Two mechanisms, neither of which relies on review:

1. **Static.** No package under `server-go/modules/` other than `egress` may reference
   `net.Dial`, `http.Client`, or socket syscalls. This is mechanically checkable, in the
   same spirit as `check-module-boundary.py`.
2. **Runtime.** Module processes run with no network namespace (`--network none`). The
   delegate sandbox already does exactly this, so the mechanism is proven in this codebase
   rather than borrowed on faith.

The static check is what keeps the rule visible during development; the runtime constraint
is what makes it true. Neither alone is sufficient — a lint can be worked around, and a
namespace restriction discovered at runtime is a bad way to learn about a design mistake.

### Credentials

Because `egress` is the only component that dials, it is the only component that needs a
credential. Provider and forge secrets stop propagating to callers. This extends an
instinct the codebase already has: the git path deliberately passes its token on a memfd
so it never lands in a child's environment, and PR operations call the forge API in-process
specifically to avoid handing a token to a subprocess.

## 3. What makes this feasible

The transport already carries what is needed for ordinary calls. `ModuleMessageMaxBody` is
16 MB and bodies are assembled across frames, which comfortably covers request/response
traffic — provider metadata, forge API calls, webhooks.

Handlers are also free to block. `RunModuleProcess` dispatches each handler with `go
runHandler(...)`, and cancellation and deadlines are owned by the transport, so a handler
may park on a slow external call without stalling its process and without inventing a
timeout policy of its own.

## 4. What makes it hard

**Streaming is the deciding constraint.** LLM traffic is long-lived SSE. A 16 MB ceiling is
irrelevant to a token stream that must arrive incrementally over minutes; what matters is
that the module protocol is request/response. Egress therefore needs a chunked response
shape.

This is a known quantity here rather than a research problem — the detached provider
already models `{"partial":true,...}` responses, and the workspace runner handoff
(`StageRunnerIO`) carries a streaming discipline — but it is the part to prototype before
committing to the design.

**A single egress is a shared bottleneck.** One module serving every outbound call means a
slow provider can head-of-line-block git. Per-target queues and bounded concurrency address
it, but this introduces a scheduling problem that does not exist today.

**Blast radius grows.** Egress down means nothing external works at all, where today a
broken git path does not affect LLM calls.

**Non-HTTP protocols are where this leaks.** git over SSH is the immediate case. A design
that quietly exempts it reopens precisely the hole it exists to close — and one exception
is enough to end the guarantee, because "all egress is audited" with an asterisk is just
"most egress is audited". This is the proposal's largest cost and its main virtue in the
same breath: it forces the awkward protocols to be solved rather than excused.

## 5. Sequencing

The ban is the **end state**, not the opening move. Declaring it before `egress` covers
every protocol in use would either break working paths or force exceptions, and the first
exception ends the guarantee.

1. **Authorize and audit only.** A module asks `egress` whether it may call a target and
   receives a decision plus a credential handle; the record is written. This closes the
   silent-omission hole immediately, at low cost, with no streaming risk. Modules still
   carry the bytes.
2. **Move unary byte paths.** Plain request/response HTTP first — forge APIs, provider
   metadata, webhooks.
3. **Move streaming.** LLM calls, once the chunked response shape is measured under real
   token rates.
4. **Close the door.** Add the static lint, then `--network none`. Only once every protocol
   in use is covered, including git over SSH.

Each step is independently useful, and the guarantee only hardens at step 4.

## 6. Acceptance

- A module cannot reach an external host except through `egress`, verified by the static
  check and by a module process running with no network namespace.
- Every outbound call appears in the audit tap, with no code path capable of omitting it.
- Provider and forge credentials exist only in `egress`.
- Streaming LLM traffic runs through `egress` at production token rates without buffering
  a whole response.
- git over SSH is covered, not exempted.

## 7. Open questions

- Does `egress` own retries, backoff, and circuit-breaking, or does the calling module keep
  them? Centralizing gives uniform behaviour; keeping them local avoids one policy for very
  different callers.
- Is per-target isolation a queue, a goroutine pool, or a separate process per target class?
- How is a credential handle scoped and revoked so that "give me a handle for target X"
  cannot be widened by the caller?
- Does the audit record carry request/response bodies, or only metadata and a content hash?
  Bodies are the most useful for governance and the most dangerous to retain.
