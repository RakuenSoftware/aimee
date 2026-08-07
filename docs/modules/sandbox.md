# sandbox module

## Purpose and non-goals

`sandbox` owns the **learned toolchain** for delegate sandboxes: the apt packages a
project turned out to need, captured from what delegates actually ran and recorded
against the project's git root so the next sandbox image pre-bakes them.

A delegate sandbox is `--network none` by intent, so its toolchain has to exist in the
image at build time. Rather than make every project author a toolchain spec, aimee
learns the set: when a delegate runs `apt-get install <pkgs>` inside its sandbox, the
package names are captured and unioned into that project's set.

It does not build images, choose base images, or execute anything. It parses a command
and owns a store; the delegate sandbox image builder consumes the result.

## Public contracts

Two stages, both carrying JSON in each direction. A shell command and a git root are
variable-length, so the fixed binary framing the pure-decision stages use does not fit.

| Stage | Kind | Request | Response |
|---|---|---|---|
| `sandbox-learned-observe` | 10753 | `{git_root, command}` | `{parsed, recorded, packages}` |
| `sandbox-learned-load` | 10754 | `{git_root}` | `{packages}` |

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
command a delegate ran, and `delegates`, whose `delegate_sandbox_image.c` calls
`sandbox_learned_load` to pre-bake the learned set into the next image.

## Providers and readiness

The module runs as a separately supervised Go process attached to the local bus, hosted
by the `aimee-module` multicall binary. It is **optional**, so it is not part of the
required set that holds the listener out of rotation: a deployment without it starts
normally and simply never learns a toolchain.

Attachment is the readiness signal. When the module is not attached, `observe` is
skipped and `load` yields an empty set, which degrades image builds to the un-augmented
base image rather than failing them.

## Configuration and activation

- `runtime_toggle.supported`: `true`; the module is operator-controlled at runtime.

The descriptor sets `enabled_by_default: true`, so a shipped image runs it unless the
operator says otherwise. `AIMEE_MODULE_SANDBOX=0` turns it off and `=1` turns it on,
read at container start by `deploy/container/optional-modules-lib.sh`, which rewrites a
copy of the shipped manifest rather than editing the baked one.

Capture is additionally gated in core by `delegate.sandbox.learn_packages`. With the
module running but the gate off, nothing is recorded.

## Surfaces

There is no HTTP route, CLI command, or console page. The module's entire surface is the
two bus stages above plus the C entry points `sandbox_learned_observe` and
`sandbox_learned_load` declared in `aimee/sandbox/module_api.h`.

The operator-visible surface is indirect: the packages that appear in a generated
sandbox Dockerfile, and the resulting content-hash image tag.

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

## Supported journeys

A delegate runs `apt-get install ripgrep jq` inside its sandbox to finish a task. `tools`
hands the command to `observe`, which parses the two names and unions them into the
project's set. The next time that project builds a sandbox image, `load` returns
`["jq","ripgrep"]` sorted, the builder bakes them in, and the delegate finds them present
in a `--network none` sandbox.

The second journey is an operator turning learning off with `AIMEE_MODULE_SANDBOX=0`
after deciding a project's toolchain should be pinned by hand instead.

## Tests and failure behavior

`src/tests/test_sandbox_pkg_proxy.c` covers the C-side proxy, and
`server-go/modules/sandbox/sandbox_test.go` covers the parser, the package-name grammar,
and the store.

Learning is best-effort and must never fail a delegate turn. A missing, oversized
(> 1 MiB), or unparseable store reads as empty rather than as an error, and an
unattached module means the capture is skipped, not that the turn fails. A malformed
request is answered with an invalid-request status rather than a partial write.

## Operational diagnostics

`observe` returns `{parsed, recorded, packages}`, so a caller can distinguish "the
command was not an apt install" (`parsed` zero) from "it was, and nothing new was added"
(`recorded` zero). That distinction is the first thing to check when a package a delegate
installed does not appear in the next image.

Inspecting `sandbox-learned.json` under `AIMEE_HOME` shows the recorded state directly;
an absent key for a git root means nothing was ever captured for that project.

## Compatibility

The two event kinds are fixed by the process-contract formula and are not renegotiable
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

Removal is clean because the module is optional and its data is derived: turning
`AIMEE_MODULE_SANDBOX=0` leaves image builds on the un-augmented base image, and deleting
`sandbox-learned.json` leaves no residue. Nothing else reads the store.
