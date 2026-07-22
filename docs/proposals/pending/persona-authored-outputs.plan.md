# persona-authored-outputs — implementation plan

Mechanics for [persona-authored-outputs.md](persona-authored-outputs.md).
Section numbers follow that document's What items (B8 here covers its
items 8–9, the enforced tooling and the commit gate; B9 → item 10,
B10 → item 11). Paths are under `src/` unless noted; workflow-engine files
live in `src/workflow/`.

## A1 — permission table and grant resolution

- New `delegate_role_permissions(role)` in `server/delegate_role.c`
  returning a bitmask (`PERM_READ|PERM_WRITE|PERM_EXECUTE`). The table is
  keyed on **canonical** role names (aliases resolve first via
  `delegate_role_canonicalize`, so `test`→`validate`, `research`→`execute`
  need no rows of their own). Rows seed today's behavior:
  `code`, `refactor`, `prose`, `line-edit`, `lyric`, `hook` → rwx (the
  current `delegate_role_is_write()` list, `delegate_role.c:54`);
  `validate`, `execute` → r+x (they run commands);
  `review`, `diagnose`, plus any canonical role with no row and no
  template → r.
- On-disk override: `permissions: [read, write]` frontmatter in
  `<config>/role_templates/<role>.md`, parsed next to `max_turns:`
  (`role_template_max_turns`, `role_templates.c:468`). Unknown token →
  template load error; absent/empty grant → the table row, else `[read]`.
- Grants are resolved once at dispatch setup and carried with the
  dispatch; gate sites read the carried grant — no file I/O in
  `server_compute.c` / `agent_runtime.c` hot paths.
- Migrate every `delegate_role_is_write()` caller, then delete it:
  `server_compute.c:1024,1167`, `agent_runtime.c:238,439,482`,
  `agent_fallback.c:92`, `server_http.c:207`, `agent_loop.c:249`,
  `cmd_agent_delegate.c:1715`.

## A2 — capability from grant

- `agent_runtime.c:234-238`: `use_tools` gains "or the grant carries
  write|execute" alongside the name heuristic, so
  `write_capable = use_tools && (grant & PERM_WRITE)` cannot self-cancel.
  Invariant check before landing: no built-in write role self-cancels
  today (each already reaches tools by name or explicit `--tools`); if one
  does, this step is a behavior fix, not a no-op, and is called out in the
  commit.
- `delegate_role_result_cache_enabled`, `delegate_default_max_turns_for_role`,
  `delegate_final_after_turns_for_role` stay name-keyed (economics).
- The review-evidence and diff-bundle injections
  (`server/delegate_prompt.c:1188,1486,1840`) keep their review-family
  gate and additionally require a read-only grant: **read-only AND
  review-family**, never read-only alone. Keying purely off read-only
  would drag every read dispatch — including B6's PR-text dispatch — into
  the diff-evidence guard, whose drift check can fail the dispatch.

## A3 — coverage matching

- Persona frontmatter: `requires: [read, ...]` parsed in `persona.c`
  `load_file()` (additive key, like `roles:` today); built-ins get values
  in `g_builtins` (`persona.c:103`): reviewers `[read]`, engineer
  `[read, write, execute]`, novel/songwriter `[read, write]`.
- A delegate agent's granted set = union of its advertised roles' grants
  (`roles[]`/`exec_roles[]`, `delegate_role.c:24`).
- Workflow producing path: agent selection today does **no** persona/role
  matching — `wfe_resolve_delegate` (`server/wfe_delegate_resolve.c:42`)
  resolves `$random`/named only, and `wfe_live_delegate_run` dispatches
  whatever it returns. Coverage enforcement lands at that selection point:
  the resolver (or the agent pick immediately after it) filters candidates
  by `granted ⊇ persona.requires`. A workflow-named delegate that fails
  coverage is rejected with a named dispatch error, not silently accepted;
  no eligible delegate at all fails the dispatch the same way the no-agent
  case does today.
- Workflow judge path: `wfe_live_judge_run`'s agent loop
  (`server/wfe_live_delegate.c:315-321`) is the one place that walks
  `pinfo.roles[]`/`agent_supports_persona` today. It migrates to the same
  coverage predicate when `roles[]` retires — left as-is it would see
  `roles_count == 0`, choose no agent, and every judge dispatch would
  fail closed.
- CLI path: `delegate_agent_supports_role` filtering gains the same
  coverage predicate.
- `persona_t.roles[]` and its frontmatter key are parsed but ignored
  (deprecation note in docs). `check_role`/`check_marker` unchanged.
- `delegates: full|readonly|none` ceiling enforcement point unchanged
  (`cmd_agent_delegate.c`), now expressed as max grantable set
  (full→rwx, readonly→r, none→∅).

## A4 — persona from context, grant through the provider

- `cmd_agent_delegate.c:630`: `--persona` no longer required; if present,
  warn "deprecated, ignored". Resolution: workflow node `persona:` param
  (threaded per B) → session persona (`config_current_persona()`) →
  `config.default_persona` → `engineer`.
- MCP `delegate` tool (`mcp_tools.c:606`): `persona` drops from required;
  if supplied, ignored with a warning in the tool result.
- `wfe_delegate_dispatch` (`workflow/wfe_blocks.c:636`) and the provider
  contract `wfe_delegate_provider_t`'s `run()` gain **two** params: an
  explicit persona and the resolved grant bitmask (mock providers grow
  them too: `test_wfe_delegate_seam.c`, `test_wfe_manager_flow.c`).
  `wfe_live_delegate_run` (`server/wfe_live_delegate.c:75`) stops reading
  the role slot as the persona (`:116`); the role slot means a
  permission-role again. Inside the live provider the grant is enforced,
  not advisory: tools are gated on `write|execute`, and the auto-commit
  block (`git add -A` + `git commit --no-verify`,
  `server/wfe_live_delegate.c:183-185`, unconditional today) is skipped
  entirely when the grant lacks `write` — a read dispatch must not sweep
  pre-existing staged changes into a commit.

## A5 — generated stance and prose re-authoring

- `persona_compose_delegate_prompt()` (`persona.c:496`) gains the granted
  bitmask; emits the stance block from it. Callers threaded:
  `server/wfe_live_delegate.c:117,289`, `server/wfe_live_panel.c:184`,
  `delegate_ensemble.c:1117`, `delegate_prompt.c:1812`,
  `cmd_agent_delegate.c:841`, test stubs (`test_server_compute.c:488`,
  `test_delegate_ensemble.c:44`, `test_persona.c`).
- Re-author in `prompts.c`: remove the stance sentence from each reviewer
  identity paragraph (`prompts.c:309,340,373,408,439,491`), the read-only
  `## Delegation` blocks, and every literal
  `aimee delegate <role> --persona` example (engineer :54, primary :108,
  novel :152, songwriter :192, reviewers :331,364,398,430,482,518).
  `persona_delegation_block()` (`persona.c:135`) regenerates without
  `--persona`.
- Seeded-file migration: `persona_install_defaults()` (`persona.c:611`,
  skip-if-exists at :626) starts writing a `seed_version:` frontmatter
  key. Because every existing install's files carry no version and the
  re-authoring removes the old prose from the binary, the release
  **embeds a frozen table of the v0 seed content hashes** (one per
  built-in persona, captured from the pre-change generator before the
  prose edit lands). On startup seeding (`server_seed_config.c:79`): file
  with no `seed_version` whose hash matches its v0 entry → pristine
  legacy seed, re-seed it; hash mismatch → user-edited, leave it and log;
  `seed_version` current → skip.

## B6 — pr.open text

- `exec_pr_open` (`workflow/wfe_blocks.c:1343`) pre-step: dispatch persona
  = node `persona:` param or `technical-writer`, grant `[read]`, prompt
  carries `git log <base>..<branch>` + `git diff --stat`. Artifact path:
  `mkstemp` under the run's scratch dir, out-of-tree. The read grant makes
  the provider skip tools-with-write and the auto-commit (A4), so nothing
  can land on the branch.
- Validate: non-empty single-line title, non-empty body. Do not try to
  predict the forge's post-quoting caps (`shq` into `etitle[512]` /
  `ebody[1024]`, `server/wfe_live_forge.c:264-280`): call `open()` with
  the authored text; on -1, retry once with (work-item, "") before
  returning `wfe_step_looped()`.

## B7 — document persona

- `exec_document` (`workflow/wfe_blocks.c:1044`): persona =
  `node_str(node, "persona")`, default `engineer` (pattern:
  `wfe_blocks.c:965`). `config/workflows/build.yaml` document node gains
  `persona: technical-writer`.

## B8 — enforced git tooling and the shared commit gate

- Enforcement config: new key `require_aimee_git` (default 1), the
  `require_aimee_memory` pattern exactly (`config.c:325` schema, `:734`
  default, `:1196` load). Operator opt-out in `aimee.yaml` only; no
  env-var bypass (same stance as `require_session_worktree`). The verb
  set is not a new list: both layers reuse the guardrails orchestrator's
  tokenizing detectors — `bash_has_git_write`
  (`guardrails_orchestrator.c:243,342`; quote- and `git -C`-aware),
  `bash_has_gh_pr_create` (`:406`), and the push gate (`:329`) — extended
  with `tag` and `stash` so every verb with a guarded MCP equivalent
  (`server_mcp.c:1392` table) is redirected to it. Read verbs
  (`status`, `log`, `diff`, …) pass untouched. The refusal is a redirect
  nudge, not a sandbox (`sh -c '…'` smuggling is out of scope; the
  AI-attribution ban stays the hard outer guard).
- Enforcement layer 1, interactive sessions: `cli_attention_guard.c`
  (a Claude Code PreToolUse hook; Bash text scanning precedent at `:112`,
  `:719`) refuses matching commands with a message naming `git_commit`,
  `git_push`, `git_pr`.
- Enforcement layer 2, in-process delegates: the attention-guard hook
  never sees `agent_execute_with_tools` delegates
  (`delegate_backend_local.c:234` runs raw `/bin/bash -c`), so the same
  refusal lands in the server-side agent policy intercept
  (`agent_policy.c`, alongside the existing `forbidden_commands` /
  discovery-shell intercepts), using the same orchestrator detectors.
  Without this layer the enforcement would cover only interactive
  sessions and the self-commit caveat would return for exactly the
  autonomous dispatches this proposal exists to fix.
- Out of the gate by design: aimee-server's own subprocess git (the
  forge's push and `gh pr create`, `server/wfe_live_forge.c:289-303`;
  `source.archive`'s chore commit on the run branch) — internal calls,
  not agent shells. The .md scopes the one-gate claim to agent-authored
  commits accordingly.
- The shared commit gate: `handle_git_commit` (`mcp_git_write.c:53`) is
  refactored so its stage→guard→commit core (AI-attribution strip,
  main-branch block, branch ownership, sensitive-file skip) is callable
  in-process **with an explicit workdir**: today every git call inside it
  goes through `mcp_git_run`/`get_current_branch`, which key off the
  process-global `run_cmd_get_cwd()` (`mcp_git_query.c:84,289`). The
  harness caller pins `run_cmd_set_cwd(workdir)` around the core — the
  exact pattern `wfe_live_verify_run` already uses for the same reason
  (`server/wfe_live_delegate.c:218-227`) — so the gate commits in the
  per-work-item worktree, not the daemon checkout (whose branch could
  even be `main`, tripping the gate's own guard). The workflow harness
  commit (`server/wfe_live_delegate.c:183-185`, hardcoded
  `-m "aimee: autonomous delegate change"` today) then calls the core
  instead of raw `git_run`.
- Persona authorship at the gate — skip-on-draft, agent-only: the
  authoring pass runs only when (a) `commit_style_persona` is set and the
  persona has a voice, (b) the commit is agent-originated (MCP callers
  and the harness; a human operator's CLI commit passes through
  unchanged), and (c) no persona-styled draft accompanied the commit (a
  handoff-supplied `commit_message` from a styled producing dispatch is
  committed as supplied — never a second authoring pass; the honest
  rationale for the style block is fallback quality, not gate cost). The
  pass dispatches the persona read-only over `git diff --cached` plus the
  caller's draft. Failure, no backend, empty reply, or a reply that
  `strip_ai_attribution` reduces to empty → the draft commits as
  supplied. Nothing staged → the gate errors before any dispatch, as
  `git commit` does today.
- Re-entrancy of the nested pass: the MCP-side gate dispatches through
  the same in-process entry the workflow uses
  (`agent_execute_with_tools`, read-only agent config), and the gate
  saves/restores the thread-local state the inner dispatch clobbers —
  `run_cmd` cwd and the `g_write_capable` TLS (`agent_tools.c:58`) —
  re-pinning the workdir before the final `git commit`. Recursion is
  bounded structurally: the authoring delegate is read-granted, so it has
  no `git_commit` tool and cannot re-enter the gate.
- Draft channel for harness commits, discriminated by dispatch kind so
  artifact blocks keep raw text: committing dispatches (`implement`
  plain, fanout units, TDD, `document`) get the `delegate_result_v1`
  contract appended to their prompt; the contract gains a
  `commit_message` field — `delegate_handoff_append_contract`
  (`delegate_prompt.c:151`) adds it to the contract text (today:
  `schema_version, status, changed_files, tests, summary`, no message
  carrier), and the parser (`delegate_handoff_validate_text` →
  `delegate_handoff_validation_t`, `headers/cmd_agent_delegate_impl.h`)
  retains the string (today it keeps only counts and status; the field
  stays optional in the validator so plan-packet handoffs keep
  validating). The provider parses `res.response` before the free at
  `server/wfe_live_delegate.c:183` and passes `commit_message` to the
  gate as the draft. Artifact dispatches (`author.*`, B6's PR text) keep
  `res.response` verbatim.
- Delegate self-commits route through the gate too: with
  `require_aimee_git` on, a delegate's own commit is an MCP `git_commit`
  call, so its message gets the same persona pass — the round-4
  best-effort caveat disappears.
- Style block (draft quality, keeps the gate's pass cheap): producing
  dispatch prompts (`implement` plain `wfe_blocks.c:1011`, fanout units
  `:888` — note the fixed `char prompt[1024]` there needs growth, TDD
  RED/GREEN `:932,:969`, `document` `:1048`) append
  "## Commit message style" = `commit_style_persona`'s voice text when
  non-empty.
- Config key `commit_style_persona[64]`: `headers/config.h` (next to
  `default_persona:279`), default + load in `config.c` (pattern of
  `default_persona` at :842, :1076), descriptor `config_fields.c:22`
  pattern, save `config_save.c:356` pattern.
- PR side of the gate: `handle_git_pr` (`mcp_git_pr.c:160`) gets the same
  treatment as B6's `pr.open` — an absent/empty title or body is
  persona-authored over the branch evidence before the create, with the
  same fail-open fallback. Note the create path today hard-rejects an
  empty title (`'title' parameter is required`, `mcp_git_pr.c:515`); that
  check relaxes to "author, then require".

## B9 — voice field

- `persona_t` gains `voice_text`; `extract_section` list (`persona.c:407`)
  gains `## Voice`; `persona_install_defaults` writes it; built-in
  fallback: the tech-writer voice prose moves from
  `PROMPT_TECH_WRITER_TEXT` (`prompts.c:466`) into a
  `prompt_persona_voice(mode)` accessor (NULL for personas without one).

## B10 — validation

- `wfe_def_validate` (`workflow/wfe_validate.c`): for `implement`,
  `document`, `pr.open` nodes with a `persona:` param, resolve against
  built-ins + persona files; unknown → named validation error. (The
  validator gains a resolver hook so the pure-def path stays testable.)

## Test map (one row per acceptance bullet)

| Acceptance bullet | Test |
|---|---|
| table reproduces write set; override honored; unknown token rejected | `test_delegate_role.c` new cases |
| write grant honored end to end (tool-enable × write_capable) | `test_server_compute.c` runtime case through `agent_runtime` write path |
| coverage matching, both dispatch paths; named-delegate miss rejected | `test_delegate_ensemble.c` (CLI path) + `test_wfe_delegate_seam.c` (workflow routing) |
| `--persona` deprecation warning; no `--persona` in composed prompts | `test_cmd_delegate.c` warning case + prompt-scan assertion in `test_persona.c` |
| stance across grants; stale-seed re-seed vs edited-file preserved | `test_persona.c` (compose with `[read]` vs `[read,write]`; fabricated v0 file with no `seed_version` key, hash-match and hash-mismatch cases) |
| PR text happy path; garbage/empty fallback; open() −1 retry-with-empty; no extra commit | `test_wfe_blocks.c` with new capturing delegate stub + forge recorder |
| `require_aimee_git` refuses raw git write / `gh pr` on both layers; off passes; read verbs pass | `test_attention_guard.c` (hook layer) + agent-policy intercept cases beside the orchestrator detectors' existing tests |
| MCP `git_commit` and harness commit share the gate, in the caller's worktree; hardcoded message gone | `test_wfe_blocks.c` (harness path, workdir pinning) + MCP handler test via the shared core |
| gate-authored vs skip-on-draft vs human pass-through; dispatch-failure / strips-to-empty → draft; empty key → pass-through; config round-trip | gate unit test with stubbed persona dispatch + `test_config.c` round-trip |
| voiceless persona no-op; `## Voice` parsed | `test_persona.c` |
| validate rejects unknown persona on the three node kinds | `test_wfe_validate.c` (new file; existing def-validate coverage lives in `test_workflow.c`) |
| no AI-attribution trailers on either path | existing `strip_ai_attribution` coverage + commit-path equivalent in `test_wfe_blocks.c` |

## Sequencing

1. A1 (permission table + gates) — mechanical; the table reproduces
   today's sets by construction.
2. A2 (tool-enable from grant, injection gates) — behavior-affecting;
   guarded by the self-cancel invariant check above.
3. A5 prose re-authoring + composer grant param, with A4's
   dispatch-persona/grant threading through the provider contract.
4. A3 matching + `requires:`.
5. B8 in two steps: the shared commit gate + draft channel first, then
   `require_aimee_git` enforcement (the gate must exist before raw git is
   refused, or agents have no commit path). B9 voice, B7 document,
   B6 pr.open, B10 validation.
6. Docs (personas.md, DELEGATES.md, WORKFLOW_ACTIONS.md, CLI help,
   generated command reference, the git-tooling requirement).
