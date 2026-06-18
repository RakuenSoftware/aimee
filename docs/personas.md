# Personas

A **persona** is the primary agent's identity, its system-prompt voice, craft
principles, session-brief hints, the delegate roles it advertises, and its
**delegate policy**. Personas are server-owned and user-extensible: aimee ships
eight built-ins and seeds them as editable files, and you can add your own.

## Built-in personas

| Persona | Role | Delegate policy |
|---|---|---|
| `engineer` (default) | Autonomous software engineer | **full**, must delegate multi-file/infra/parallel work |
| `novel` | Fiction author / worldbuilder | **readonly**, writes prose itself; read-only delegates only (continuity, beat-check, review) |
| `songwriter` | Songwriter / lyricist | **none**, does all the work itself; no delegates |
| `qa` | Senior QA / test reviewer | **readonly**, reviews and reports; read-only delegates only (review, diagnose, validate, research) |
| `security` | Senior application-security reviewer | **readonly**, reviews and reports; read-only delegates only (review, diagnose, validate, research) |
| `reviewer` | Senior **contrarian** code reviewer (thorough, comprehensive) | **readonly**, reviews and reports; read-only delegates only (review, diagnose, validate, research) |
| `reviewer-constructive` | Senior **constructive** code reviewer (assess as written) | **readonly**, reviews and reports; read-only delegates only (review, diagnose, validate, research) |
| `architect` | Software-architecture reviewer | **readonly**, reviews and reports; read-only delegates only (review, diagnose, validate, research) |

The five reviewer personas (`qa`, `security`, `reviewer`,
`reviewer-constructive`, `architect`) reframe the agent as a read-only senior
reviewer: it investigates and surfaces findings with evidence, never editing the
code. They share a common **Review Principles** block and each carry their own
review methodology. `reviewer` and `reviewer-constructive` are a deliberate pair
— the former is adversarial (tries to refute), the latter assesses the work as
written — so a roundtable panel can blend both stances.

## Where personas live

Single markdown files, resolved project → user → built-in:

- Project: `<project>/.aimee/personas/<name>.md`
- User: `~/.config/aimee/personas/<name>.md`
- Built-in fallback
  (engineer/novel/songwriter/qa/security/reviewer/reviewer-constructive/architect)

`aimee init` seeds the built-ins as editable files under
`~/.config/aimee/personas/` (idempotent, it never overwrites an existing file),
so they are self-documenting starting points you can copy and edit.

## File format

YAML frontmatter + markdown sections:

```markdown
---
name: noir-detective
description: Hard-boiled detective narrator
delegates: readonly          # full | readonly | none  (default: full)
roles: [continuity, beat-check, review, research]
check_role: continuity       # done-gate delegate for `aimee manuscript check`
check_marker: CONTINUITY      # verdict stem (CONTINUITY: PASS|FAIL)
---
## Persona
You are a hard-boiled detective novelist working in %s.
Write in terse, atmospheric first-person...

## Principles
# Craft Principles
- Keep the voice clipped and cynical...

## Brief
- Recall the case facts before writing a scene.
```

- `%s` in **Persona** is replaced with the working directory.
- All sections and frontmatter keys are optional; sensible defaults apply
  (an unknown persona resolves to `engineer`; an omitted `delegates` is `full`).

## Delegate policy (config-controlled)

The `delegates` frontmatter key controls whether and how the agent may use
`aimee delegate`. It is enforced two ways, a strong instruction in the agent's
prompt, **and** a hard refusal at the command:

- **`full`**, the agent is told it MUST delegate multi-file changes,
  infrastructure, deployments, and parallel work. All roles allowed.
- **`readonly`**, the agent does writing/editing itself and may use **read-only
  delegates only** (review/checks). `aimee delegate <write-role>` is refused.
- **`none`**, `aimee delegate` is refused entirely; the agent does all work
  itself.

In every persona the **Agent tool is disabled** (it is removed from the agent's
toolset); aimee delegates are the only sub-agent mechanism.

## Using a persona

- **In the OpenCode TUI:** `/persona <name>` switches the session's persona.
  `/persona` (no argument) shows a **numbered list** to pick from, reply with a
  persona's number (and press Enter) to switch to it, or type `/persona <name>`.
  `/engineer`, `/novel`, `/songwriter` are aliases. The persona is stored
  per-session on the server.
- **In webchat:** the persona is per-session too. The webchat server exposes
  `GET /api/chat/personas` (the selector list) and `GET`/`POST /api/chat/persona`
  (read / set the session's persona), proxied to aimee-server's `/v1` API over
  its Unix socket. A set persona is sticky for that browser session, so both the
  chat system prompt and delegate-policy enforcement honor it, exactly like a
  TUI session.
- **Durable default:** `aimee init --novel` / `--songwriter` sets the default
  persona for the install (written to `~/.config/aimee/mode`).
- **Inspect:** over the V1 API (below), `GET /v1/personas`.

## Assigning a persona to a delegate (required)

Every delegate runs **as a persona**; it is **required**, not optional. The
persona sets the delegate's identity and principles; for a non-engineer
built-in or a custom persona, its identity prose (the "You are …" body) is
layered onto the role template, so e.g. a `review` delegate run as `security`
reviews with the security reviewer's methodology rather than the generic
engineer framing.

- **CLI:** `aimee delegate <role> --persona <name> "prompt"`. Omitting
  `--persona` is an error. An unknown persona name warns and falls back to the
  engineer principles.
- **MCP `delegate` tool:** `persona` is a **required** property alongside `role`
  and `prompt`.

The persona name resolves from the built-ins or a user-level persona file (the
same set as the rest of the persona surface). The delegate's *role* still
governs tool access and write capability; the *persona* governs its identity and
principles, and they compose.

## Personas in roundtables and workflows

A **roundtable** review panel is staffed by personas: each panelist is a persona
run on a delegate model, and the panel pairs personas to models reproducibly run
to run (the contrarian `reviewer` and constructive `reviewer-constructive` make a
natural adversarial/constructive split). The default review panel is
`security`, `architect`, `qa`, `reviewer`.

In a [workflow](WORKFLOWS.md), a `gate.roundtable` step names its panel by
persona, and any action step can be assigned a persona/delegate pair:

```yaml
- id: review
  block: gate.roundtable
  params:
    panel:
      required: [security, architect, qa, reviewer]
    quorum: 4
```

The webchat **Workflows** tab includes a persona manager (create/edit personas)
and a per-step persona picker, both backed by `/api/chat/personas`. So the same
eight built-ins — plus any you add — are the vocabulary for chat identity,
delegate assignment, roundtable panels, and per-step workflow roles alike.

## V1 HTTP API

Personas are reached over aimee-server's `/v1` HTTP API (Unix socket
`~/.config/aimee/aimee-http.sock`), clients never read the files directly:

| Method & path | Returns |
|---|---|
| `GET /v1/personas` | list (`name`, `description`, `builtin`) |
| `GET /v1/personas/<name>` | full persona (roles, check_role, check_marker, delegates, builtin) |
| `GET /v1/persona` | the active durable-default persona |
| `GET /v1/sessions/<id>/persona` | the session's persona (else durable default) |
| `POST /v1/sessions/<id>/persona` `{"name":...}` | set the session's persona |
