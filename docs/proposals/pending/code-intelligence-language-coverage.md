# Proposal: Language coverage for code intelligence — reach the grammars we already ship, then extend

- **State:** PENDING — design only, no code in this PR. Motivated by the
  aimee-encoder corpus contract, which was extended from 30 to 48 code streams
  (build/config, mainstream gaps, infra DSLs). The encoder learns to *retrieve*
  over those languages; this proposal is about Aimee's own extraction and
  indexing reaching the same set, so retrieval and symbol lookup agree on what
  counts as code.
- **Author:** JBailes
- **Date:** 2026-07-29
- **Charter roles:** Execute (collector/extractor extension), Persist (index and
  symbol records for new languages), Review (per-language acceptance evidence
  before a grammar is declared supported).

## Problem and boundary

Aimee's code intelligence classifies files by extension in two places that do
not agree with each other:

- `src/code_treesitter.c` maps 44 extensions onto **23 compiled grammars**: c,
  cpp, c_sharp, python, go, javascript, typescript, tsx, rust, java, ruby, php,
  lua, bash, swift, kotlin, dart, css, scala, groovy, objc, elixir, powershell.
- `src/code_collect.c` collects **27 extensions**.

The comment in `code_collect.c` states the two extend "in lockstep so the
collector never sends an extension the extractor can't parse." Neither direction
actually holds, and the expensive one is measured below.

**Nine grammars are compiled in and unreachable — C# among them.** Twenty
extensions parse today and are never collected:

```
.cjs .cs .css .cts .dart .ex .exs .gradle .groovy .kts
.lua .mjs .mm .mts .php .ps1 .psm1 .sc .scala .zsh
```

Every extension of `CSHARP`, `CSS`, `DART`, `ELIXIR`, `GROOVY`, `LUA`, `PHP`,
`POWERSHELL`, and `SCALA` is in that list, so those nine grammars cannot return
a single result from `aimee index find`. We pay their binary size and build
dependency for nothing. A C# repository indexed by Aimee today yields no C#
symbols at all.

The reverse gap exists too: `.hxx` is collected but has no extractor entry
(`.hpp`/`.hh`/`.cxx` do). `.md`, `.yaml`, `.yml`, `.toml`, `.json` are also
collected without an extractor, but that is deliberate — they feed the
build-declared-edge pass as content.

That is the first half of the problem and it needs no new dependency to fix.

The second half is genuine absence. Against the encoder's 48-stream contract,
Aimee has no extraction for: haskell, zig, erlang, ocaml, fortran, julia, r,
perl, solidity, sql, html, clojure, fsharp, vue, svelte, hcl, nix, protobuf,
graphql, and the build/config tier (cmake, make, dockerfile, yaml, toml, json,
markdown). Some of the build/config set is *collected* as content for the
build-declared-edge pass but has no extractor, so it contributes edges and text
but no symbols.

**Boundary.** This proposal covers file classification, collection, and symbol
extraction. It does not change the retrieval ranking, the graph contract, or the
encoder's corpus mix; those are separate artifacts.

## Decision

Three bounded slices, ordered so the cheapest real gain lands first.

**S1 — Reach the grammars we already have.** Extend the collector's extension
list to the full set the extractor can parse, add the missing `.hxx` extractor
entry, and replace the two hand-maintained lists with one table that both sites
read. Two lists that must agree, maintained by comment, is the defect; one table
removes the class. No new grammar, no new dependency, no binary growth — this
slice alone restores nine languages including C#.

**S2 — Build and configuration as first-class.** Add `cmake`, `make`,
`dockerfile`, `yaml`, `toml`, `json` classification, including the filename-only
forms (`CMakeLists.txt`, `src/Makefile`, `Dockerfile.server`, `Containerfile`).
Aimee is itself a CMake + Docker + compose-YAML project: a 68 KB
`CMakeLists.txt`, dozens of compose files, and `aimee.yaml` are core artifacts
that symbol search cannot currently see as code. Symbol extraction for these is
target/variable/service level, not function level — a CMake `add_library`, a
compose service name, a YAML anchor.

**S3 — The remaining languages, by evidenced demand.** haskell, zig, erlang,
ocaml, fortran, julia, r, perl, solidity, sql, html, clojure, fsharp, vue,
svelte, hcl, nix, protobuf, graphql. Each is a tree-sitter grammar addition plus
an extractor query set. These are not equal in value and should not land as one
batch; see the ordering rule below.

## Non-goals

- No language is declared supported on the strength of a grammar linking. A
  grammar that parses but extracts nothing useful is worse than an absent one:
  it produces confident empty results.
- No MATLAB. `.m` is already claimed by Objective-C in both the extractor and
  the encoder contract, and the ambiguity is not worth resolving for a language
  neither system otherwise serves.
- No vendored or generated content. Lockfiles, minified bundles, and generated
  sources are excluded by name, as the encoder contract now does.
- This does not propose retiring any currently supported language.

## Ordering rule

S3 languages land in the order that evidence supports, not alphabetically and
not by my guess. The ordering input is the encoder corpus itself: after the 10B
shard build, per-language unique-token capacity is a measured quantity, and the
`aimee index` corpus gives a second signal — how many files of each extension
actually exist in the repositories this instance serves. A language with neither
corpus volume nor local files waits.

## Failure model

- **Silent misclassification.** `.m` (Objective-C vs MATLAB), `.t` (Perl vs
  Terraform test), `.sc` (Scala vs SuperCollider), `.fs` (F# vs GLSL fragment
  shader). Each ambiguous extension needs an explicit decision recorded in the
  table, not a first-match-wins accident.
- **Generated-file flooding.** One `package-lock.json` can outweigh a hand-written
  package. Exclusion is by name before size, because a size cap admits many small
  generated files.
- **Grammar version drift.** A tree-sitter grammar that outpaces its query set
  parses successfully and extracts nothing. Per-language acceptance must assert a
  symbol count from a fixture, so a silent regression fails the build.
- **Binary and build cost.** Every grammar is compiled in. Nineteen more is a
  material size increase on a binary that ships to appliances; S3 should measure
  the delta per grammar and state a budget before it is spent.

## Compatibility and migration

Existing indexes remain valid. New extensions produce new records; no stored
record changes shape. An index built before S1 will not contain `.php` or `.lua`
symbols until re-indexed, so the slice ships with a re-index note rather than an
implicit expectation that existing deployments pick it up.

## Acceptance checks

**Mechanical (per slice):**
- One table is the single source of extension → language; a test asserts the
  collector and extractor derive from it and cannot diverge.
- For each newly reachable or added language, a fixture file yields a non-zero,
  asserted symbol count. Not "parses" — extracts.
- Ambiguous extensions have an explicit recorded owner and a test proving the
  loser is not silently accepted.
- Generated/lockfile names are excluded, proven by fixture.

**Integration:**
- `aimee index find <symbol>` returns results from a `.php`, `.lua`, and `.ps1`
  fixture repository after S1 — the direct proof that the unreachable grammars
  are reachable.
- After S2, `aimee index find` locates a CMake target and a compose service
  defined in Aimee's own tree.
- Binary size delta reported per slice against a stated budget.

## Status

PENDING. S1 is self-contained and carries no new dependency; it is the slice to
review first. S2 depends only on S1's unified table. S3 is deliberately unsized
until the 10B corpus build reports measured per-language capacity.
