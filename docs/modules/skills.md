# skills module

## Purpose and non-goals

Skills are required core instructions and supporting resources that teach Aimee how to perform recurring
work under explicit project or user control. The module resolves, loads, validates, injects, reviews, and
maintains `SKILL.md`-style content; it is not an optional extension loader, a tool implementation, or an
unbounded mechanism for downloaded text to bypass routing, policy, or user approval.

## Public contracts

The canonical contracts are
`src/modules/skills/include/aimee/skills/skill.h` and
`src/modules/skills/include/aimee/skills/skill_review.h`, implemented by `skill.c`, `skill_review.c`,
and `skill_rollback.c` in `src/modules/skills`, with CLI orchestration in `src/cmd_skill.c`. Consumers
use the `aimee/skills` include namespace. The former singular source directory is retired without a
forwarding API or parallel skill registry.

`src/modules/skills/module.yaml` declares ownership of the three production sources above, both
canonical public headers, two direct unit tests, and this document; the module has no private headers.
Its `ownership_complete: true` latch exhaustively checks module-local C and private-header files and
requires this canonical document. Public-header and test entries are explicit ownership claims, but the
completeness latch does not discover undeclared public headers or tests. Command, server, protocol, and
configuration files with skill surfaces remain orchestration or consumer boundaries rather than
duplicate skill implementations. The source-level liveness and public-API cleanup audit is recorded in
`docs/validation/core-modularization-slice-33.md`.

## Dependencies and consumers

- `config`: supplies dispatch, index, lifecycle, and evaluation policy for skills.
- `ir`: supplies the provider-neutral conversation context in which skill instructions are applied.
- `memory`: records skill evidence and usage needed for retrieval, learning, and review.
- `module-runtime`: supplies the required lifecycle contracts shared by all core modules.
- `tools`: supplies the capability registry against which skills can declare and cover operations.

Consumers include `cmd_agent_delegate.c`, session briefing, `cmd_review.c`, `cmd_skill.c`, workflow and
delegate execution, client integration installers, and capability autostub logic. They must use the same
`skill_path`, `skill_load`, lint, usage, and management contracts.

## Providers and readiness

The filesystem-backed resolver is the required provider and searches project then user scopes according
to `skill_path`; absence of a named skill is a normal lookup miss. Evaluation or proposal generation may
use other modules, but core readiness requires deterministic parsing, linting, bounded loading, and
injection even when no optional plugin or web GUI is present.

## Configuration and activation

- `runtime_toggle.supported`: `false`; skills are core while individual dispatch and lifecycle policies remain configurable.

Settings such as `skills_dispatch_enabled`, index limits, evaluation threshold, and stale/archive windows
tune automated journeys. Disabling dispatch must not remove manual `aimee skill` management or the
underlying contracts, and GUI controls must be hidden when their actual consumer is absent.

## Surfaces

The primary user surface is `aimee skill` with list, show, lint, eval, create, edit, patch, archive,
export, import, rollback, lifecycle, autostub, pin, and unpin verbs. Project `.aimee/skills`, user skill
directories, delegate prompt injection, session briefings, and review payloads are also supported
surfaces with bounded content and stable precedence.

## Data and migrations

Skill bodies and support files are filesystem data; usage, state, snapshots, and review records use
sidecars or repository databases according to the current implementation. Canonical source ownership
under `src/modules/skills` does not change project-over-user precedence, archive behavior, size limits,
or rollback snapshot interpretation.

## Security and privacy

Skill names and support paths must pass `skill_name_is_valid` and containment checks so content cannot
escape approved project/user roots. Loaded instructions are untrusted input subject to execution policy,
tool authorization, and review gates; pinning protects automation state but does not grant the skill
additional capabilities or access to secrets.

## Supported journeys

A user can author and lint a project skill, review it, activate it explicitly or through bounded dispatch,
and have `skill_inject` add its instructions to the appropriate delegate prompt. Usage telemetry supports
lifecycle decisions, while evaluation gates and rollback preserve a recoverable path for automated skill
changes derived from learning or uncovered tools.

## Tests and failure behavior

The descriptor owns `test_skill.c`, which covers discovery, loading, validation, management, injection,
lifecycle, rollback, and telemetry, and `test_skill_review.c`, which covers the review predicate and
poison checks through the management contract. Invalid names, oversized content, unsafe support paths,
failed lint/evaluation, and write errors fail closed; a missing requested skill leaves the base prompt
usable rather than inventing instructions.

## Operational diagnostics

Use `aimee skill list|show|lint|eval`, review records, and lifecycle result counts to diagnose
precedence, malformed content, stale state, or failed automation. Diagnostics should include the resolved
source class and safe path context while avoiding logging full private skill bodies or injected secrets.

## Compatibility

CLI verbs, `skill_*` C signatures, project/user lookup precedence, frontmatter interpretation, file size
limits, and snapshot semantics are compatibility contracts. The canonical directory and include
namespace preserve stored skill files and the external `SKILL.md` convention.

## Extension and removal

New lifecycle or authoring features must extend the single resolver/management implementation and add
tests for scope, safety, review, and rollback. Duplicate client-specific skill engines should be folded
into `skills`; an isolated feature with no production consumer is deletion evidence. Removing skills is
not an allowed core profile because delegates and learning depend on durable, user-controlled guidance.
