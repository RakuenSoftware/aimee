# Proposal: Effective agent tool scoping through aimee's existing toolset seams

- **State:** REJECTED — superseded C-only delegate execution seam; archived 2026-08-15.
- **Date:** 2026-07-21.
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Thesis:** aimee already resolves role toolsets, filters advertised tools, and checks
  role membership again in the native tool dispatcher. Tighten those existing seams so
  one turn-local resolved name list is used for both disclosure and execution in the POSIX
  server-delegate path. Adjacent
  authorities remain unchanged and outside this proposal. Add no runtime, graph, service, registry, schema surface,
  policy object, approval vocabulary, persistence path, audit vocabulary, or CLI family.

## Decision

Rejected under the Go-or-rejected implementation policy. The prescribed implementation adds
private `_Thread_local` capability state to `src/server/agent_tools.c`, binds it from
`src/server/server_compute.c`, and proves it through C test targets. That was a design for the old
POSIX server-delegate execution path.

Delegate execution has since moved into the Go `delegates` module (`5273532f96`). The module now
owns role-permission resolution, the permission-to-tool clamp, CLI construction, container
lifecycle, and sandbox enforcement. Adding the proposed C snapshot would create a second delegate
authorization owner and violate the current module boundary.

This rejects the obsolete placement, not the resolve-once safety invariant. Any remaining
disclosure/dispatch parity work must be specified against `server-go/modules/delegates` and its
provider adapters, then implemented and tested there. The remainder of this document is retained as
the rejected C design record; its named symbols and acceptance targets were not implemented.

## 1. Problem

aimee already has the right pieces, but they are evaluated independently:

- `toolset_resolve_effective()` resolves a named toolset;
- `agent_tools_filter_for_role()` resolves and filters the provider tool array;
- `agent_tools_tool_allowed_for_role()` resolves again at invocation;
- `dispatch_tool_call_ctx()` / `dispatch_tool_call_ctx_inner()` are the native execution
  choke point that already invokes the membership check;
- `tools_enabled` is the agent-level all-or-nothing switch;
- the active toolset is already turn-local;
- persona permissions, per-block workflow policy, governance posture/approval, identity,
  and sandbox isolation each have their own current or pending owner.

The remaining gap is small but load-bearing: disclosure and execution should consume the
same resolved list for the same turn. Re-resolving independently makes drift possible when
thread-local selection or registry availability changes between request construction and
dispatch.

This proposal does not make tool scoping a new authority. It makes the existing toolset
decision consistent at both places where aimee already uses it.

## 2. Verified machinery to reuse

| Concern | Existing aimee seam | Change here |
| --- | --- | --- |
| Toolset definition and includes | `src/toolset.c` | None |
| Effective resolution | `toolset_resolve_effective()` | Resolve once for the turn |
| Turn-local selection | `g_active_toolset` in `src/server/agent_tools.c` | Carry the resolved names beside the current selection in the same translation unit |
| Tool disclosure | `agent_tools_filter_for_role()` | Filter from the turn-local resolved names |
| Invocation check | `agent_tools_tool_allowed_for_role()` | Check the same turn-local names |
| Native dispatch | `dispatch_tool_call_ctx()` / `dispatch_tool_call_ctx_inner()` | Preserve the existing membership call site |
| Agent off switch | existing `tools_enabled` handling | No change |
| Guardrails/governance | existing governance-owned path | No change |
| Approval | `wfe_approval_*` and governance-owned `require_approval` work | No change |
| Lifecycle/audit | existing tool-call/lifecycle events | No new event kind or field |
| Workflows | current WFE and pending per-block policy work | No change; deferred |
| Personas | pending permission-role work | No change; deferred |
| Sandbox | workspace/container providers and sole-egress work | Remains a lower, independent boundary |

## 3. Exact behavior

### 3.1 Resolve once for the existing membership checks

At the existing server-compute binding site, extend the current turn-local toolset binding
to carry the ordered result of `toolset_resolve_effective()`. The binding receives the
already-available explicit override, role, and tools-on/off decision. It applies the same
selection inputs the two consumers use today: explicit turn-local override, existing CLI
environment channel, then `toolset_for_delegate_role(role)`. The change centralizes that
existing choice; it does not introduce another precedence source.

The storage stays beside `_Thread_local g_active_toolset` in
`src/server/agent_tools.c:74`. The implementation adds only private thread-local siblings:
`g_effective_tool_names`, `g_effective_tool_count`, and `g_effective_tool_state`. The two
internal transitions are `agent_tools_bind_effective_toolset()` and
`agent_tools_clear_effective_toolset()`. No public/end-user type, serialization, hash,
revision, database representation, loader, registry, or API is added.

One internal bind helper replaces the production setter call. On every call it first
zeroes the full name-array capacity, then overwrites count and flag, even if a pooled worker
was returned without a prior clear. One internal clear helper unconditionally sets the
flag to `unbound`, sets count to zero, and zeroes the full name-array capacity, including
when already unbound; the existing unconditional cleanup calls it. These are single-thread
transitions over thread-local data: no consumer reads count or names while the flag is
`unbound`, and no separate setter may update only part of the binding.

The binding is explicitly tri-state:

- `unbound` — legacy/non-turn caller; current fallback behavior is unchanged;
- `bound_empty` — a tools-on turn whose effective toolset resolves to zero names;
- `bound_list` — a real turn with the resolved names.

The bound flag is distinct from the name count, so an empty authorized surface can never
fall through to the legacy default. Existing `tools_enabled: false` behavior remains the
agent runtime's all-or-nothing off path: the helper routes it directly through `clear()`
and never creates a bound state or invokes the resolver.

For a tools-on turn, a negative result from `toolset_resolve_effective()` first completes
the `bound_empty` state transition (zero full array, zero count, set flag) and only then
logs the existing warning. It is not a warning-only path and cannot fall through to the
legacy role fallback. Both disclosure and execution then deny every name. A successful
zero-name resolution is also `bound_empty`. The helper snapshots a successful list at bind
time and never re-resolves it during that turn.

The required implementation order is fully specified below. This is proposal pseudocode;
the source implementation has not started:

```text
bind(tools_on, explicit_override, role):
  if not tools_on: clear(); return
  state = bound_empty                 # old list is unreachable immediately
  memset(names, 0, sizeof(names))     # full declared capacity, never only count slots
  count = 0
  selected = explicit_override || existing_cli_channel || toolset_for_delegate_role(role)
  n = toolset_resolve_effective(selected, scratch, capacity, error)
  if n < 0: warn(error); return       # tools-on only; state is fully bound_empty
  copy names[0..n) from scratch
  count = n
  state = (n == 0 ? bound_empty : bound_list)

clear():
  state = unbound
  count = 0
  memset(names, 0, sizeof(names))     # full capacity, including clear-on-unbound
```

Consumers iterate only `[0, count)`. A capacity-sized list followed by a shorter bind
therefore makes the old tail both unreachable by count and byte-zeroed by the full-array
`memset`.

Persona, workflow, authenticated-scope, and governance composition is deferred completely
to their owners. This proposal defines no cross-authority union/intersection rule and adds
no `toolsets`, `limits`, permission, or policy field anywhere.

### 3.2 Disclosure uses the resolved names

`agent_tools_filter_for_role()` already filters both OpenAI-shaped tool surfaces after the
single ordered definition table is rendered. Change its membership source from a fresh
toolset resolution to the turn-local resolved names.

The current loop deletes rejected array elements in place and advances only over survivors
(`src/server/agent_tools.c:567-577`), so filtering already preserves survivor order. This
proposal adds no IR stage, provider filter, or adapter obligation.

### 3.3 Execution uses the same resolved names

`dispatch_tool_call_ctx_inner()` continues to invoke
`agent_tools_tool_allowed_for_role(active_role, name)` at its current call site. This
proposal changes only the membership data source inside that check:

- `unbound` uses today's fallback path unchanged;
- `bound_empty` denies every name;
- `bound_list` uses `strcmp` exact equality against the snapshotted names without resolving
  or consulting an alias/registry lookup again. Any input not exactly present denies.

After the existing null/empty tool-name validation, the first policy branch reads the
thread-local bound flag. `bound_list` returns the exact-equality result, `bound_empty`
returns deny, and only `unbound` reaches today's resolver/role fallback. Bound states have
no control-flow edge to `toolset_for_delegate_role()`, `toolset_resolve_effective()`, an
alias map, or a registry lookup. The `toolset_for_delegate_role(role)` call in the bind
pseudocode is selection input before the snapshot is created, not a bound-consumer edge.

The snapshot has no caller-supplied construction path: it contains only the exact strings
returned by `toolset_resolve_effective()`. Dispatch compares those opaque strings with
`strcmp` and introduces no alias, identity, or liveness lookup.

Both `agent_tools_filter_for_role()` and `agent_tools_tool_allowed_for_role()` read the same
thread-local state, count, and name array. While `bound_list` or `bound_empty`, neither has
a separate resolver or role-fallback edge.

No ordering around guardrails, governance, approval, lifecycle hooks, or handlers changes.

## 4. Hard precondition: evidence-only seam inventory

Slice 0 is a review gate and authorizes no runtime, dispatcher, adapter, registry, or schema
edit. The inventory below is the result of that gate; line numbers are from the reviewed
tree and symbols are the durable anchors.

| File:line | Symbol / role | Class | Evidence and decision |
| --- | --- | --- | --- |
| `src/server/agent_tools.c:74-86` | `g_active_toolset`, setter/getter; turn-local carrier | `in_scope` | Existing `_Thread_local` storage. Private bound flag, count, and name-array siblings live here; flag and count are independent. |
| `src/server/server_compute.c:677-714` | binding inputs | `in_scope` | Explicit override, canonical role, and `force_tools` are all available before execution. Existing consumer fallbacks are override, CLI environment, then role mapping. |
| `src/server/server_compute.c:1248` | active-toolset bind | `in_scope` | The only production setter. Resolve once here when tools are on. |
| `src/server/server_compute.c:1506-1508` | active-toolset clear | `in_scope` | Unconditional pooled-thread cleanup also clears the sibling state. |
| `src/server/agent_tools.c:445-487` | `agent_tools_tool_allowed_for_role` | `in_scope` | Currently resolves independently. When bound, replace only its membership source and retain the existing post-membership review-worktree reachability check. Resolver/role fallback is reachable only when the flag is `unbound`. |
| `src/server/agent_tools.c:540-579` | `agent_tools_filter_for_role` | `in_scope` | Currently resolves independently, then deletes in place without reordering survivors. Use the bound names; `bound_empty` emits zero tools. |
| `src/posix/agent_runtime.c:556-564` | disclosure consumer | `in_scope` | The provider-neutral runtime builds one provider-shaped array and calls the filter before request construction. |
| `src/modules/tools/agent_tools_dispatch.c:1878-1964` | dispatch membership call | `in_scope` | `dispatch_tool_call_ctx_inner()` calls the allowed predicate at line 1958. The call site and surrounding order remain unchanged. |
| `src/posix/agent_runtime.c:1550-1551` | model-loop dispatch caller | `in_scope` | Server delegate execution is bracketed by the bind/clear above; this call consumes the same thread-local list. Other legacy callers remain `unbound`. |
| `src/server/script_rpc.c:179-188` | script-RPC dispatch caller | `owner_required` | Script RPC has its own resolved `script_allowed_tools` check before dispatch. It remains deliberately `unbound`; no script-RPC edit is authorized. |
| `src/modules/tools/agent_tools_dispatch.c:2227-2229` | generic dispatch wrapper | `owner_required` | Direct non-turn callers remain `unbound` and retain legacy behavior. No wrapper edit is authorized. |
| `src/windows/agent_runtime.c:298-327` | Windows dispatcher | `owner_required` | Separate platform implementation has no named POSIX membership seam. Invariants 1-2 apply only to the POSIX server-delegate Slice 1. The Windows agent-runtime owner must approve a separately gated parity slice before those invariants can be claimed there. |
| `src/cmd_agent_delegate.c:705-715` | CLI validation resolution | `owner_required` | Preflight validates an explicit CLI toolset and passes it through the existing environment channel. This is not disclosure or execution membership and remains unchanged. |
| `src/cmd_agent_delegate_toolset.c:38-62` | CLI argument disambiguation resolution | `owner_required` | Read-only parsing/validation use; remains unchanged. |

The symbol inventory is replayable from the repository root. Each query below is
read-only; the expected counts include declarations and tests so additions cannot hide.

| Kind | Anchor and replay command | Expected result |
| --- | --- | --- |
| `symbol` | `rg -n '^_Thread_local static char g_active_toolset' src/server/agent_tools.c` | one definition in `src/server/agent_tools.c` |
| `refs` | `rg --files-with-matches 'g_active_toolset' src --glob '*.[ch]'` | exactly `src/server/agent_tools.c`; callers use the setter/getter, not the private variable |
| `refs` | `rg -n 'agent_tools_set_active_toolset\(' src --glob '*.[ch]'` | production bind/clear only in `src/server/server_compute.c`; remaining matches are declaration, definition, and tests |
| `symbol` | `rg -n '^_Thread_local static .*g_effective_tool_names' src/server/agent_tools.c` | exactly one definition after Slice 1 |
| `symbol` | `rg -n '^_Thread_local static .*g_effective_tool_count' src/server/agent_tools.c` | exactly one definition after Slice 1 |
| `symbol` | `rg -n '^_Thread_local static .*g_effective_tool_state' src/server/agent_tools.c` | exactly one definition after Slice 1 |
| `symbol` | `rg -n '^.*agent_tools_bind_effective_toolset\(' src/server/agent_tools.c` | exactly one definition after Slice 1 |
| `symbol` | `rg -n '^.*agent_tools_clear_effective_toolset\(' src/server/agent_tools.c` | exactly one definition after Slice 1 |
| `refs` | `rg -n 'agent_tools_(bind|clear)_effective_toolset\(' src/server/server_compute.c` | exactly two production calls after Slice 1: one bind and one clear |
| `refs` | `rg --count-matches 'agent_tools_filter_for_role\(' src --glob '*.[ch]'` | 6: header 1, implementation 1, runtime 1, tests 3 |
| `refs` | `rg --count-matches 'agent_tools_tool_allowed_for_role\(' src --glob '*.[ch]'` | 25: header 1, implementation 2, dispatcher 1, tests 21 |
| `refs` | `rg --count-matches 'dispatch_tool_call_ctx_inner\(' src --glob '*.[ch]'` | 3, all in the POSIX dispatcher: declaration, wrapper call, definition |
| `symbol+refs` | `rg --count-matches 'toolset_resolve_effective\(' src --glob '*.[ch]'` | 7: implementation 1, header 1, consumers 4, test stub 1 |
| `symbol+refs` | `rg --count-matches 'toolset_for_delegate_role\(' src --glob '*.[ch]'` | 14: implementation 1, header 1, consumers 2, tests/stub 10 |

Resolver inspection confirms `resolve_index()` suppresses duplicate returned strings
(`src/toolset.c:466-480`) and `toolset_resolve()` sorts the unique result at
`src/toolset.c:486-513`. This slice adds no naming or identity behavior.

Current test ownership is also explicit:

| Existing target | Current coverage | Extension in Slice 1 |
| --- | --- | --- |
| `unit-test-toolset` | Resolver uniqueness/order and role-set contents in `src/tests/test_toolset.c:23-169` | Pin the list mirrored from one effective resolution; resolver behavior is unchanged. |
| `unit-test-agent` | Both predicates and provider-shaped filtering in `src/tests/test_agent.c:483-521` | Add bound-list, bound-empty, unbound, and clear assertions using the same fixtures. |
| `unit-test-agent-request-build` | Canonical provider request byte stability in `src/tests/test_agent_request_build.c:1-96` | Regression-only: proves the change does not alter canonical request bytes; tool membership stays in `unit-test-agent`. |

The `owner_required` rows above have a recorded no-change disposition and therefore do not
block the POSIX server-delegate slice. Any later attempt to bind those paths requires their
owner's separate approval; Slice 1 cannot absorb them.

## 5. Hook-in with current proposals

- [Persona-authored outputs](persona-authored-outputs-residual.md) owns the `read/write/execute`
  permission model and exact write-tool set. **Consumed shape here:** none; a later
  owner-approved adapter may narrow the active toolset before this proposal resolves it.
- The shipped primary-as-manager workflow and pending workflow-policy work own block
  parameters, validation, versions, routing, and tool stripping. **Consumed shape here:**
  none; no workflow field or validator is assumed.
- [One governance policy surface](governance-policy-surface-and-posture.md) owns
  `observe|standard|hardened`, admission, allow/deny/require-approval, rollout evidence,
  and human gating. **Consumed shape here:** none; current call ordering is unchanged.
- [Attestable enforcement](governance-attestable-enforcement.md) owns audit event shape,
  policy revision, and the WORM chain. **Consumed shape here:** none; events are unchanged.
- [Agent identity and artifact trust](governance-agent-identity-and-artifact-trust.md) and
  [thin-client mTLS](../done/tiered-llm-p8-thinclient-mtls.md) own principals, delegation identity,
  authentication, revocation, and connection caps. **Consumed shape here:** none.
- [IR as the sole path](ir-sole-path-residual.md) owns canonical request
  routing. **Consumed shape here:** none; canonical routing remains unchanged.
- [Response/orchestration stages](../done/response-orchestration-stages.md) and the
  [delegate first-port plan](../done/orchestration-seam-delegate-firstport.md) own turn/capability
  handles. **Consumed shape here:** only the existing turn-local binding lifetime.
- Provider-neutral cache-aware economizer work owns cache accounting. **Consumed shape
  here:** none; provider tool-array order remains
  unchanged by filtering.
- [Delegate sandbox sole egress](delegate-sandbox-aimee-sole-egress.md) and
  [sandbox image customization](delegate-sandbox-image-customization.md) own process,
  filesystem, credential, and network boundaries. **Consumed shape here:** none.
- [Local-first memory and trust patterns](local-first-memory-and-trust-patterns.md) owns
  non-owner trust defaults and semantic retry deduplication. **Consumed shape here:** none.
- [Config field descriptors](../pending/config-field-descriptor-save-residual.md) owns future Go
  editable flat-scalar metadata and persistence convergence. **Consumed shape here:** none; no
  field is added.

## 6. Sequencing

### Slice 0 — inventory gate

- Produce and review the §4 inventory.
- Report collisions and out-of-scope paths to their owners without editing them.
- **Gate:** Slice 1 cannot start until every row is `in_scope` or its owner resolves it.

### Slice 1 — one turn-local list

- Extend existing active-toolset thread-local state with the resolved ordered names.
- Make `agent_tools_filter_for_role()` and `agent_tools_tool_allowed_for_role()` consume
  that list.
- Implement and clear the explicit `unbound|bound_empty|bound_list` state.
- Clear the bound flag, count, and names at the existing unconditional clear site.

Rollout and enforcement posture remain wholly controlled by governance's existing plan.
Operators author any declarations through the owning configuration surfaces; this proposal
provides no generator, bulk migration, compatibility reporter, UI, or promotion command.

## 7. Security invariants

1. On the POSIX server-delegate path, the tool names disclosed for a turn and the names
   accepted by native dispatch are the same turn-local list.
2. On that path, `bound_empty` is distinguishable from `unbound`; it can never fall through
   to a default.
3. Output tool order equals the order of names in the input tool array that are also in
   the bound list. Rejected entries may be compacted or deleted, but surviving entries keep
   their original relative order; resolved-list order never reorders the provider array.
4. The existing resolver returns a sorted, duplicate-free list and prunes unknown names;
   this proposal does not change that contract.
5. No capability data is persisted or emitted through a new audit/approval surface.
6. This proposal changes the data source of the existing membership checks, not their call
   sites, surrounding order, or adjacent authority.

## 8. Non-goals

- New agent or workflow fields.
- A policy object, hash, revision, compiler, resolver service, registry, namespace, or
  plugin framework.
- A new dispatcher, provider filter, IR stage, persistence path, DB1 schema, approval
  scope, audit event, metric, CLI command, explain section, rollout mode, or UI.
- A side-effect taxonomy or persona permission mapping; persona work owns it.
- Workflow-block schema or validation; workflow work owns it.
- Semantic retry deduplication or exactly-once external effects.
- Windows dispatcher parity; the Windows agent-runtime owner must gate parity separately.
- New execution limits, counters, inheritance, or scheduler behavior.
- Replacing sandbox, transport authentication, governance, data-level authorization,
  liveness, cost, or output controls.

## 9. Acceptance

The implementation extends existing test targets and fixtures rather than inventing an
unbound test family:

- `unit-test-toolset` gains a fixture whose existing baseline resolves to ordered names
  `[read_file, grep, write_file]` and is mirrored without changing the resolver.
- `unit-test-agent` extends the existing role-filter tests so one cJSON tool array is
  filtered to the expected ordered names and the same names return allowed at dispatch;
  every filtered name returns denied.
- `unit-test-agent` adds a zero-name effective toolset that renders zero tools and denies
  dispatch under `bound_empty`; a separate `unbound` fixture preserves today's fallback.
  It also covers pooled reuse (bind A, bind B without a prior clear, assert only B), then
  clear and assert the unbound fallback; two concurrent bound lists cannot leak.
- `unit-test-agent` supplies an input array ordered differently from the resolved list and
  asserts survivor order follows the input. Reversed input, duplicate input, and
  all-suppressed fixtures pin the same relative-order rule. It also asserts aliases and
  names absent from the snapshot deny under `bound_list`.
- `unit-test-agent` covers tools-on resolution failure: zero tools are disclosed and every
  dispatch name denies. It covers clear-while-unbound as an idempotent no-op that remains
  `unbound`, distinct from `bound_empty`.
- `unit-test-agent` binds a capacity-sized list, then a shorter list without clearing and
  proves every prior tail name denies. Null and empty input names deny before state-policy
  branching; for the same valid name the fixture pins `bound_list` exact membership,
  `bound_empty` denial, then `unbound` legacy equivalence.
- `unit-test-agent` uses a resolver stub that would allow a name and proves neither
  `bound_empty` consumer calls it. Empty-input and all-suppressed arrays produce zero
  output while leaving the bound state unchanged for the next predicate assertion.
- `unit-test-agent` covers bind, negative resolution, unconditional clear, then a non-empty
  bind: only the new names survive, and an unbound consumer after the clear observes the
  legacy fallback. `tools_enabled: false` enters `clear()` and emits no resolution warning.
- `unit-test-agent` names three paired parity cases under the existing target:
  1. `bound_list`: interleave filter and allowed checks against one binding; both
     yield the identical allowed set.
  2. `bound_empty`: filter yields zero names and allowed denies every valid input
     before any resolver or role fallback.
  3. `shorter-overwrite`: replace a capacity-sized bind with a shorter bind; both
     consumers expose only the shorter set and every prior tail name denies.
  These are the complete paired parity suite. Pooled-worker reuse assertions elsewhere in
  this section are separate state-lifecycle coverage, not a fourth parity case.
- `unit-test-agent-request-build` remains a byte-stability regression target; no tool-array
  assertion or new provider hook is added there.
- Existing governance, WFE approval, lifecycle, and sandbox tests remain byte-for-byte
  compatible because this proposal adds no verdict, state, event, field, or boundary.

```yaml acceptance
- {id: 1, tier: mechanical, check: "make build/obj/tests/unit-test-toolset"}
- {id: 2, tier: mechanical, check: "make build/obj/tests/unit-test-agent"}
- {id: 3, tier: mechanical, check: "make build/obj/tests/unit-test-agent-request-build"}
```
