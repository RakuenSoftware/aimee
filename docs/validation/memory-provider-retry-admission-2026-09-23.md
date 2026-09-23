# Provider retry admission — 2026-09-23

The native HTTP retry helper could resend a frozen provider body after backoff
without asking whether its retained memory sources were still eligible. The
first provider fence alone did not cover mutations or owner outages during the
wait. Provider callers now install an explicit admission callback that relays
the existing Go source check after backoff and before each resend. Ordinary HTTP
requests retain their unguarded transport, so an owner's revalidation request
cannot recursively enter this callback.

Go continues to own source eligibility, exact revisions and request-bound opaque
handles. The C retry loop and wire fence only carry the admission result. A
refusal clears the previous provider response and returns a distinct local status;
it produces neither another provider call nor another provider-failure event.
Tool and non-tool agent execution preserve `context_refused` across primary and
model-fallback calls, without entering provider fallback, health accounting or
context compression. Buffered Responses calls preserve the specific refusal kind.
The non-tool path now also checks inherited source commitments before its first
send and before a model fallback.

[Socket tests](memory-provider-retry-admission-2026-09-23/http-retry.txt) verify
unchanged exact-length bytes, including an embedded NUL, across an admitted retry;
they also verify a refusal after backoff sends only the first request and leaves
no stale provider response. [Native ingress tests](memory-provider-retry-admission-2026-09-23/ingress.txt)
use the real Go memory fixture to check unchanged versions, intervening source
changes, malformed owner answers and owner failure between admissions.
[Agent tests](memory-provider-retry-admission-2026-09-23/agent.txt) exercise both
execution paths and their model fallbacks with the local refusal result. All pass.
The provider fixture comes from the real Go provider module. The local GCC build
uses `-std=gnu17`; the host compiler's C23 default exposes an unrelated existing
const-qualified `strchr` call in the CLI. No CLI workaround is included here.

The deployment fixture additionally injects one transient provider failure for
buffered Responses using both OpenAI and Anthropic provider formats, checking
successful recovery and identical serialized bytes. Fresh candidate validation
is pending. These checks do not establish durable dispatch receipts, full
provider-path parity, or elimination of the mutation race after the owner's
snapshot. MR-06 remains open.
