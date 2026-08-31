# sandbox module

## Purpose and non-goals

`sandbox` owns the **learned toolchain** and package-egress policy for delegate
sandboxes. It records apt packages a project needed so the next image pre-bakes them,
and decides which proxy requests and resolved addresses are safe to reach.

A delegate sandbox is `--network none` by intent, so its toolchain has to exist in the
image at build time. Rather than make every project author a toolchain spec, aimee
learns the set: when a delegate runs `apt-get install <pkgs>` inside its sandbox, the
package names are captured and unioned into that project's set.

It does not build images or choose base images. The Go package parses commands, owns the
learned store and immutable registry allowlist, resolves every destination once, rejects
non-public addresses, dials the validated numeric address, strips credential/hop headers,
and forwards bounded bytes. The C server only transfers an accepted AF_UNIX fd and the
already-read request bytes to `aimee-delegate-egress`; it opens no outbound socket and
makes no destination decision.

## Public contracts

Four stages, all carrying JSON in each direction because their strings are
variable-length.

| Stage | Kind | Request | Response |
|---|---|---|---|
| `sandbox-learned-observe` | 10753 | `{git_root, command}` | `{parsed, recorded, packages}` |
| `sandbox-learned-load` | 10754 | `{git_root}` | `{packages}` |
| `sandbox-proxy-request-policy` | 10755 | `{line, allowlist?}` | `{kind, host, port, allowed, forward_head?}` |
| `sandbox-proxy-address-policy` | 10756 | `{ip}` | `{blocked}` |

The kinds are fixed by the process contract at `4096 + ordinal*256 + stage`; sandbox is
ordinal 26, so they are not a free choice. The public header
`aimee/sandbox/module_api.h` is the only surface core links against; a call body is
capped at `SANDBOX_CALL_MAX_BODY` (256 KiB).

`load` returns the set **sorted**, so the derived Dockerfile, and therefore the
content-hash image tag, is stable regardless of insertion order.

## Dependencies and consumers

- `audit`: carries sandbox degraded-isolation events onto the observability bus rather than a direct write.
- `config`: supplies the `delegate.sandbox.learn_packages` gate that decides whether capture runs at all.
- `module-runtime`: supplies the supervised process lifecycle, bus attachment, and capability state.

Consumers are `tools`, whose `agent_tools.c` and `agent_tools_dispatch.c` capture the
command a delegate ran; `delegates`, whose `delegate_sandbox_image.c` calls
`sandbox_learned_load` to pre-bake the learned set into the next image; and the Go
`aimee-delegate-egress` transport, which directly uses the same package policy.

## Providers and readiness

The module runs as a separately supervised Go process attached to the local bus, hosted
by the `aimee-module` multicall binary. It is **required** because every delegate
depends on its learned-toolchain and package-egress policy. A missing or unattached
module is a startup/readiness failure and keeps the listener out of rotation.

Attachment is the readiness signal. Individual learning calls remain best-effort, but
proxy policy fails closed: no classification or address approval means no outbound
connection.

## Configuration and activation

- `runtime_toggle.supported`: `false`; sandbox is part of the required runtime.

There is no sandbox on/off setting and no supported `AIMEE_MODULE_SANDBOX` environment
variable. Every deployment starts it from the required module manifest.

Capture is additionally gated in core by `delegate.sandbox.learn_packages`. With the
gate off, nothing is recorded; this changes learning only and does not disable sandbox
isolation or package-egress policy.

## Surfaces

There is no public HTTP route, CLI command, or console page. The four bus stages remain
for module consumers. Live proxy traffic enters only through the server's UDS demux and
is handed to the Go helper; C retains only the learned-package bus adapter.

The operator-visible surface is indirect: the packages that appear in a generated
`sandbox` Dockerfile, and the resulting content-hash image tag.

## Data and migrations

One store, `sandbox-learned.json` under `AIMEE_HOME`, a flat JSON object keyed by git
root whose values are package-name arrays. Bounds are `LearnMax` 128 packages per
project and `PackageNameMax` 64 bytes per name; a store larger than 1 MiB is refused.

There is no migration and no schema version. The document is derived data that can be
deleted at any time: the next delegate turn re-learns it. Writes are atomic (unique
temp plus rename), so a crash cannot truncate the store into place.

## Security and privacy

The input is an **untrusted, delegate-authored shell command**. The tokenizer is
deliberately a best-effort, quote-unaware lexical splitter: it does not interpret
quotes, backslash escapes, command substitution, expansion, or globbing. The
delegate's real shell does that.

That is safe because the **Debian package-name grammar is the security boundary**: a
token is recorded only if it is a leading alphanumeric followed by `[a-z0-9._+:-]`, so
no shell metacharacter, quote, expansion, path, or flag can be recorded. The command
word must equal exactly `apt` or `apt-get`. Names are validated again on read, so a
hand-edited sidecar cannot reach a generated Dockerfile.

Worst case a benign but wrong name is recorded, which fails its own image build and
falls back to the un-augmented base image. There is no path from this parser to shell
execution. The store holds package names and git-root paths only; it carries no command
text, no delegate output, and no credential material.

The proxy policy accepts only HTTP/1.0 or HTTP/1.1 CONNECT/absolute-form requests,
ports 80 and 443, and an exact-or-label-bounded registry allowlist. Every resolved
address is checked before the validated numeric IP is dialed. IPv4 private, loopback, link-local, CGNAT,
documentation, multicast, and reserved ranges are blocked, as are IPv6 unspecified,
loopback, unique-local, link-local, multicast, IPv4-mapped, NAT64, and 6to4 encodings
of blocked IPv4 space. Invalid policy responses and an unavailable module both fail
closed in Go. For allowed absolute-form HTTP, the module also rewrites the request to
origin form and removes hop-by-hop and credential-bearing headers before writing it upstream.

## Supported journeys

A delegate runs `apt-get install ripgrep jq` inside its sandbox to finish a task. `tools`
hands the command to `observe`, which parses the two names and unions them into the
project's set. The next time that project builds a sandbox image, `load` returns
`["jq","ripgrep"]` sorted, the builder bakes them in, and the delegate finds them present
in a `--network none` sandbox.

The second journey is an operator setting `delegate.sandbox.learn_packages: false`
after deciding a project's toolchain should be pinned by hand instead. Sandbox
isolation and package-egress policy remain active.

## Tests and failure behavior

`server-go/modules/sandbox/sandbox_test.go` covers the learned-toolchain parser,
package-name grammar, store, and stages. `proxy_policy_test.go` covers IPv4, IPv6,
ports, the immutable package allowlist, and request classification. `proxy_test.go`
covers DNS pinning, blocked addresses, request sanitization, byte/deadline bounds, and
HTTP/CONNECT forwarding. `test_delegate_egress_adapter.c` proves that the legacy server
rejects non-Unix clients and hands the Go helper the exact request bytes and inherited
Unix fd. The bus conformance suite exercises the other shipped C-host/Go-module stages.

Learning is best-effort and must never fail a delegate turn. A missing, oversized
(> 1 MiB), or unparseable store reads as empty rather than as an error. An unattached
required module prevents readiness; if attachment is lost during a call, learning
fails empty while proxy policy remains fail-closed. A malformed request is answered
with an invalid-request status rather than a partial write.

## Operational diagnostics

`observe` returns `{parsed, recorded, packages}`, so a caller can distinguish "the
command was not an apt install" (`parsed` zero) from "it was, and nothing new was added"
(`recorded` zero). That distinction is the first thing to check when a package a delegate
installed does not appear in the next image.

Inspecting `sandbox-learned.json` under `AIMEE_HOME` shows the recorded state directly;
an absent key for a git root means nothing was ever captured for that project.

## Compatibility

The four event kinds are fixed by the process-contract formula and are not renegotiable
without a contract change. The store is derived data with no schema version, so a format
change is handled by reading the old shape as empty and re-learning rather than by
migration.

The C entry points in `aimee/sandbox/module_api.h` are the compatibility surface for
core; `tools` and `delegates` call them and are the callers to check before changing a
signature.

## Extension and removal

A new stage means a new ordinal-derived kind, a handler in
`server-go/modules/sandbox/module.go`, and a descriptor update; the `ownership-complete`
latch fails CI if a module-local source or header is added without being declared.

The module cannot be disabled or removed independently: delegates have a required
dependency on it. Removing it requires an architecture and descriptor change. Its
derived `sandbox-learned.json` store may still be deleted safely to reset learned
package state.
