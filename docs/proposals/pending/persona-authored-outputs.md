# Persona-authored outputs and permission roles

## Why

Personas decide who *reviews*, not who *writes*. The prose a workflow run
ships is authored by accident or not at all: `pr.open` sends the work-item
text as the PR title and an empty body, commit messages come from whichever
producing delegate happens to run (and on the live path the harness commit
overwrites them with a hardcoded message), and the `document` block
hardcodes the `engineer` persona while `technical-writer` only reviews the
result.

The seams that would fix this are load-bearing in the wrong places. A
persona's stance is welded into its prose: every reviewer built-in says
"You report findings; you do not edit the code" in its identity paragraph,
so asking `technical-writer` to author anything composes a
self-contradictory prompt. A role's capability is a hardcoded name list
(`delegate_role_is_write()`), disconnected from what the work needs. And
delegates are addressed by (role template, persona name) pairs, so callers
hardwire *who* instead of *what capabilities the job requires*.

One model change fixes all of this. Roles become permissions. Delegates
advertise permissions. A persona declares the permissions it requires, and
any delegate that covers them is a valid target. Stance is generated from
the granted permissions instead of baked into prose. On that footing,
routing "the technical writer writes the commit messages, the PR messages,
and the docs" is config and workflow YAML, not prompt surgery.

## What

### Part A — roles are permissions

1. A role is a permission set drawn from `read`, `write`, `execute`. The
   source of truth is a built-in permission table in code, one row per
   built-in role, replacing `delegate_role_is_write()`'s name list. An
   on-disk role template may override its grant with `permissions:`
   frontmatter; unknown tokens are rejected at load and a missing or empty
   grant means read-only. Grants are resolved once per dispatch, never by
   file I/O inside a write gate. `aimee delegate <role>` keeps working: the
   role name resolves to (template, permission set).
2. Capability behavior derives from the grant. Write-gating and
   tool-enablement both follow it (a `write` or `execute` grant turns tools
   on), so a granted permission is honored end to end. The per-role
   economics heuristics (result cache, turn caps, early-final) stay
   name-keyed by design; they are cost policy, not capability.
3. Delegates are defined by roles, not personas. A delegate's granted set
   is the union of its roles' grants. A persona's frontmatter gains
   `requires:` — the permissions it needs (reviewer built-ins `[read]`,
   engineer `[read, write, execute]`). A delegate is a valid target for a
   persona iff its grants cover the persona's requirements, on both the CLI
   and workflow dispatch paths; a workflow-named delegate that fails
   coverage is rejected with a named error rather than silently accepted.
   The persona `roles:` advertisement is
   retired in favor of `requires:` plus coverage; `delegates:
   full|readonly|none` remains as the ceiling on what a persona's own
   delegates may be granted.
4. Callers cannot name a persona. `--persona` on `aimee delegate` and the
   `persona` property on the MCP `delegate` tool are deprecated: accepted
   and ignored with a warning for one release, then removed. A delegate
   runs as the dispatching context's persona — the workflow node's
   `persona:` param, else the session persona, else `default_persona`, else
   `engineer`. Context selects the persona; delegates never target one.
5. Stance is generated, not written. The composer receives the granted
   permission set and emits the stance block itself: without `write`,
   "report findings, do not edit"; with `write`, authoring framing. The
   built-in persona prose is re-authored stance-neutral — the stance
   sentences, the read-only delegation blocks, and every literal
   `--persona` example are removed from the prompt bodies. Seeded persona
   files are versioned and re-seeded so stale review-framed copies do not
   override the re-authored built-ins; the release embeds the prior seed
   content hashes so pristine legacy files are distinguishable from
   user-edited ones, which are preserved.

### Part B — persona-authored outputs

6. `pr.open` writes real PR text. The executor dispatches one
   read-granted delegate as the node's `persona:` param (default
   `technical-writer`) over the branch log and diffstat. The reply comes
   back through a temporary out-of-tree artifact file; read-granted
   dispatches never auto-commit. The executor validates the reply: first
   line becomes the title (non-empty, single line), the rest the body. On
   dispatch failure, garbage output, or a forge `open()` rejection, it
   retries once with today's arguments (work-item title, empty body).
   Opening the PR never blocks on prose, and the no-provider path is
   unchanged.
7. `document` honors a per-node persona, read the same way the `implement`
   block's TDD path already reads one, and the `build` composition sets
   `persona: technical-writer` on its document node.
8. Commit messages are delegate-authored, styled, and actually used. The
   live dispatch path today commits with a hardcoded message; committing
   dispatches will instead carry the structured delegate handoff, and the
   harness commit consumes the `commit_message` it supplies. A new config
   key, `commit_style_persona` (default `technical-writer`, empty
   disables), appends that persona's voice guidance as a
   "## Commit message style" block to every dispatch that commits:
   `implement` (plain, fanout units, TDD), and `document`. The `author.*`
   blocks produce prose artifacts, not commits, and are excluded. When a
   delegate self-commits mid-run with its own git tools, the style block
   guides it but the resulting message is best-effort.
9. Voice becomes addressable. The built-in technical-writer's voice prose
   is factored into a built-in voice field; `## Voice` joins the sections
   parsed from persona files and the seeded output. A persona without a
   voice makes `commit_style_persona` a logged no-op.
10. Persona names and grants are validated. `aimee workflow validate`
    rejects an unknown `persona:` on producing and `pr.open` nodes —
    today a typo silently falls back to `engineer` — and template loading
    rejects unknown permission tokens.
11. Docs and help follow the interface: `docs/personas.md`,
    `docs/DELEGATES.md`, `docs/WORKFLOW_ACTIONS.md`, the CLI help text,
    and the generated command reference all drop `--persona` and document
    the permission model.

Out of scope: a general action→persona binding table, and any permission
beyond `read`/`write`/`execute`. Mechanics — caller enumerations, file
lists, signatures, migration, and the test seam — are in
[persona-authored-outputs.plan.md](persona-authored-outputs.plan.md).

## Acceptance

- The built-in permission table reproduces today's write set exactly; a
  template `permissions:` override is honored; an unknown token fails the
  template load with a named error. `aimee delegate review "…"` still runs
  read-only.
- A `write` grant on a role outside today's tool-enable name list still
  yields a delegate with write tools (grant honored end to end, unit-tested
  through the runtime's write-capability path).
- Coverage matching: a `[read]` delegate is a valid target for a reviewer
  persona and an invalid target for a persona requiring `write`, on both
  dispatch paths.
- `--persona` / `persona` are accepted with a deprecation warning and
  ignored; no built-in prompt or delegation block emits `--persona`
  anywhere in the composed session or delegate prompts.
- The composed prompt for a write-granted `technical-writer` dispatch
  contains its identity and voice and no "do not edit" text; the same
  persona composed read-only contains the generated read-only stance. A
  stale seeded persona file is re-seeded on version bump and no longer
  overrides the re-authored prose.
- A `build` run opens its PR with a delegate-authored title and non-empty
  body, and no extra commit lands on the branch from the PR-text dispatch.
  With the dispatch failing, returning garbage, or the forge rejecting the
  text, the PR still opens with the work-item title and empty body —
  observed via the block-test forge recorder, not parked.
- The commit created by a live-path dispatch carries the delegate-supplied
  message, not the hardcoded one; with `commit_style_persona` set (and by
  default) the message follows the styled prompt, and the resulting
  message — not just the prompt — is asserted in tests. Config round-trips
  through save/load like `default_persona`.
- Setting `commit_style_persona` to a persona with no voice logs and
  no-ops. `## Voice` in a persona file parses into the persona.
- `aimee workflow validate` rejects an unknown `persona:` on `pr.open`,
  `document`, and `implement` nodes with a named error.
- Commit messages and PR bodies produced under the default binding contain
  no AI-attribution trailers; the existing server/CI ban stays the outer
  guard on both paths.
