# Registration-time egress classification for dynamically registered tools

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`dynamic-tool-egress-registration-identity.md`](../pending/dynamic-tool-egress-registration-identity.md).

*Filed as the known-gap record for the fail-closed egress capability gate.
Classification: **security, high**.*

> ## SEVERITY CORRECTED. There is no present bypass
>
> This record was filed as **security, high** on the reasoning that host-CLI
> tools "still resolve to permitted". Measuring the actual allowlist showed that
> claim was wrong. Every tool handed to the provider CLI is already accounted
> for: `WebFetch` and `WebSearch` are recognised as externalization, `Bash` is
> recognised as a shell tool and gated by command inspection, and the remaining
> six (`Edit`, `Read`, `Write`, `Glob`, `Grep`, `NotebookEdit`) are genuinely
> local and *should* be permitted, denying them would break gated runs outright.
>
> So the exposure was never "an egress tool is ungated today". It was temporal:
> nothing forced a tool ADDED to that list later to be classified. That is a real
> gap, but it is **medium** and it needed roughly thirty lines, not the
> registration-time metadata subsystem proposed below.
>
> **Fixed** by making the allowlist enumerable data
> (`cli_claude_allowed_tools()`) and adding `test_cli_claude_allowlist.c`, which
> forces every entry into one of three buckets, gated as externalization, gated
> by command inspection, or on a reviewed local-only list, and fails with an
> explanatory message otherwise. Verified by adding an unclassified
> egress-shaped tool to the list and confirming the suite fails.
>
> **Still open**: the own-server exemption (`mcp__aimee` / `mcp__aimee__*`) keys
> on a NAME PREFIX, not an authenticated registration identity. Scoped by
> measurement:
>
> - **Not reachable for delegates.** Their MCP config is a hardcoded literal in
>   `cli_claude.c` naming exactly one server (`aimee`), and `--allowedTools`
>   permits only `mcp__aimee`. No other server can be introduced.
> - **Reachable for primary sessions**, whose gate runs via `cmd_hooks.c` and
>   whose MCP servers the operator configures.
> - **Threat model matters.** The gate stops the *agent* externalizing before
>   delivery. The adversary that motivates it, untrusted page content steering
>   the agent, cannot register an MCP server. An operator who names their own
>   server `aimee` is bypassing their own guardrail, which is self-inflicted.
>   The genuine vector is narrower: a malicious or careless third-party MCP
>   package that names itself `aimee` and is thereby treated as in-boundary.
>   That is real but supply-chain shaped, and **low**, not high.
>
> **Why no partial fix was written.** The obvious cheap hardening. Accept
> `mcp__aimee__<tool>` only when `<tool>` is one aimee's MCP server actually
> exposes, needs a statically enumerable catalog. `MCP_CORE_TOOLS[]` in
> `mcp_tool_profile.c` is explicitly only the core profile floor ("the rest of
> the catalog is reachable via these"), so validating against it would reject
> legitimate aimee tools. A half-fix here breaks working setups while still not
> authenticating anything.
>
> The correct fix is the registration-identity work described below. It is not
> urgent at low severity, and it should not be approximated with a name check.
>
> The host-CLI half of the problem is closed.

## The gap, stated without softening

The egress capability gate is fail-closed for two populations and **still
fail-open for a third**:

| Population | Mechanism | Fail-closed? |
|---|---|---|
| Built-in tools | Declaration in `tool_egress.c`, exact-set startup invariant | **Yes**: an undeclared built-in refuses startup |
| Third-party MCP tools (`mcp__*`) | Default to EXTERNAL | **Yes**: denial is the default |
| Everything else dynamic (host-CLI tools, any non-`mcp__` registration) | Legacy name/substring lists | **No** |

For the third population the original defect survives verbatim. A tool named
`acme__lookup`, or a host-CLI tool that performs egress under a name matching no
deny substring, resolves like this:

```
tool_egress_for(name)        == TOOL_EGRESS_UNSET
tool_egress_is_external(name) == 0
is_third_party_mcp_tool(name) == 0
```

and `wfe_is_externalization_tool()` returns 0, permitted pre-delivery.

## Why it was not simply closed

The obvious fix (deny any tool that is not an explicitly declared built-in) breaks gated runs entirely. Host-CLI tools (`Read`, `Edit`, `Grep`,
`aimee_git_verify`) are not aimee built-ins, so they are UNSET. Denying UNSET at
this layer would deny ordinary file reading and editing inside a gated workflow.
The existing test suite asserts those tools are permitted, and it is right to.

The gate cannot distinguish "unknown and harmless" from "unknown and
externalizing" **because the classification does not exist at the point where
those tools are registered**. That is the actual defect, and it cannot be fixed
in the classifier, only in the registration path.

## Direction

Classification must move to registration time, so every dispatchable tool
carries an egress class regardless of origin:

1. **MCP registration** (`mcp_client_registry`): record an egress class per
   server. Absent an explicit operator declaration, EXTERNAL, which is what the
   `mcp__*` default already approximates, but enforced at registration with a
   real identity rather than inferred from a name prefix. This also removes the
   current reliance on a name prefix for the own-server exemption, which is a
   naming convention rather than an authenticated identity.
2. **Host-CLI toolsets**: declare a class per tool in the toolset definition, the
   same way built-ins do.
3. **Dispatch**: resolve the class from registration metadata, never from the
   tool name. Once every population carries a class, UNSET at dispatch becomes a
   genuine error and can be denied, retiring the legacy name lists entirely.

## Interim posture (what is true today)

The gate is strictly stronger than before: built-ins cannot be silently ungated,
third-party MCP defaults to denied, and the legacy lists can now only *add*
denials for the remaining unknown population. But "fail-closed" is **not** a
property of the whole gate until this record is implemented, and should not be
claimed as one.

## Acceptance

- Every dispatchable tool, from any registration path, resolves to a non-UNSET
  class from metadata rather than from its name.
- The own-server exemption keys on an authenticated registration identity, not a
  name prefix.
- UNSET at dispatch is denied, and a test proves a newly registered undeclared
  tool is refused rather than permitted.
- The legacy `DENY_EXACT` / `DENY_SUBSTR` lists are removed.
