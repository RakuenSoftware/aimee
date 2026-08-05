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
ordinal 26, so they are not a free choice.

`load` returns the set **sorted**, so the derived Dockerfile, and therefore the
content-hash image tag, is stable regardless of insertion order.

## Ownership

This module owns `sandbox-learned.json` under `AIMEE_HOME`, keyed by git root. The C
implementation serialised writers with a process mutex plus an `flock` across processes
sharing one home; only this module writes the file now, so a mutex is the whole story.
Writes are atomic (unique temp + rename), so a crash cannot truncate the store into
place.

The C side keeps only what is genuinely core's: the cheap prefilter that keeps an
incidental `apt` substring free, the config gate
(`delegate.sandbox.learn_packages`), and resolving a working directory's git root.

## Security model

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
execution.

## Behaviour notes

Learning is best-effort and must never fail a delegate turn. A missing, oversized
(> 1 MiB), or unparseable store reads as empty rather than as an error, and an
unattached module means the capture is skipped, not that the turn fails.
