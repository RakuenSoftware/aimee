#!/usr/bin/env python3
"""Keep modules off each other's headers: peers meet on the event bus."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


MODULES = "src/modules"
# The shared core contract every module is allowed to depend on. It is not a
# peer module and carries no peer's domain types.
CORE = "core"
# The two include roots that name a module. `aimee/<id>/` is a module's public
# API; `modules/<id>/` reaches past it into the owner's private headers, which
# the build still resolves through -Isrc. Both are counted: a peer is a peer
# whichever root and whichever bracket style names it.
MODULE_ROOTS = ("aimee", "modules")
# Including a bus client header IS bus communication, not direct coupling.
# Scoped to the exact header: audit_action.h and audit_worm.h are the audit
# module's own domain API and stay debt below.
BUS_TRANSPORT_HEADERS = {"aimee/audit/obs_bus.h"}
# Exact direct module-to-module coupling on testing, each entry a peer header a
# module reaches for in-process instead of over the bus. Closed list: nothing may
# join it, and an entry whose include is gone must be deleted, so it only shrinks.
#
# The aimee/ir/ entries are a different debt from the rest. aimee_request_t is
# the pipeline's shared data contract rather than a peer service call; retiring
# them depends on docs/proposals/pending/ir-sole-path-and-pluggable-stages.md,
# not on moving a call onto the bus.
IR_SHARED_TYPE = {
    ("src/modules/delegates/aimee_ir_rescue.c", "aimee/ir/aimee_ir_metrics.h"),
    ("src/modules/delegates/include/aimee/delegates/aimee_ir_rescue.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/delegates/include/aimee/delegates/panel_provider.h", "aimee/ir/panel_result.h"),
    ("src/modules/memory/gw_stage_memory.c", "aimee/ir/aimee_ir.h"),
    ("src/modules/memory/gw_stage_memory.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/translation/include/aimee/translation/aimee_backend.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/translation/include/aimee/translation/aimee_frontend.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/translation/include/aimee/translation/aimee_ir_stream.h", "aimee/ir/aimee_ir.h"),
}
# A peer service called directly in process. Each one must become bus traffic.
PENDING_BUS_MIGRATION = {
    ("src/modules/delegates/delegate_openai.c", "aimee/tools/agent_tools.h"),
    ("src/modules/delegates/delegate_run_phases.c", "aimee/workspace/workspace.h"),
    ("src/modules/delegates/delegate_xml_fallback.c", "aimee/tools/agent_tools.h"),
    ("src/modules/execution-policy/execution_policy.c", "aimee/protocols/mcp/mcp_client_registry.h"),
    ("src/modules/guardrails/guardrails_action_audit.c", "aimee/audit/audit_action.h"),
    ("src/modules/guardrails/guardrails_action_audit.c", "aimee/audit/audit_worm.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "aimee/skills/skill.h"),
    ("src/modules/memory/gw_stage_memory.h", "aimee/gateway/gateway_pipeline.h"),
    ("src/modules/memory/memory_assemble.c", "aimee/workspace/workspace.h"),
    ("src/modules/roadmap/roadmap_auto.c", "aimee/delegates/delegate_launch.h"),
    ("src/modules/roundtable/delegate_ensemble.c", "aimee/delegates/delegate_credentials.h"),
    ("src/modules/tools/agent_tools.c", "aimee/delegates/delegate_ephemeral_ws.h"),
    ("src/modules/tools/agent_tools.c", "aimee/protocols/mcp/mcp_client_registry.h"),
    ("src/modules/tools/agent_tools.c", "aimee/workspace/workspace.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "aimee/delegates/delegate_ephemeral_ws.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "aimee/protocols/mcp/mcp_client_registry.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "aimee/workspace/workspace.h"),
    ("src/modules/workspace/workspace_provider_container.h", "aimee/delegates/delegate_backend.h"),
}
# Worse than the above: these reach past a peer's public API into its private
# headers, so the coupling is not even to a published contract. Retiring one
# means the owner grows a bus stage for what the caller needs, not that the
# caller switches to the peer's public header.
PRIVATE_HEADER_REACH = {
    ("src/modules/git/git_ops.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/git/git_project.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/git/mcp_git_query.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/guardrails/guardrails.c", "modules/git/git_verify.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/git/git_verify.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/workspace/workspace_turn.h"),
    ("src/modules/guardrails/guardrails_tdd.c", "modules/git/git_verify.h"),
    ("src/modules/memory/memory_context.c", "modules/learning/learning_evidence.h"),
    ("src/modules/tools/agent_tools.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/tools/agent_tools_anchored.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/git/forge_credentials.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/git/git_cred_inject.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/workflows/wfe_live_forge.c", "modules/git/git_cred_inject.h"),
    ("src/modules/workflows/wfe_live_forge.c", "modules/git/git_pr_api.h"),
    ("src/modules/workspace/workspace_turn.c", "modules/git/forge_credentials.h"),
    ("src/modules/workspace/workspace_turn.c", "modules/git/git_cred_inject.h"),
}
ALLOWED = IR_SHARED_TYPE | PENDING_BUS_MIGRATION | PRIVATE_HEADER_REACH
# Both bracket styles: a quoted include couples exactly as hard as an angled one,
# and the tree uses quoted form for every `modules/` reach and for three
# `aimee/protocols/` ones.
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


class CheckError(ValueError):
    """A module reached a peer module's header instead of the event bus."""


def owning_module(relative: str) -> str:
    """The module a source path belongs to, by its position under src/modules."""
    return relative.split("/")[2]


def included_module(header: str) -> str | None:
    """The module an include names, or None when it names no module at all.

    `aimee/<id>/...` is a module's public API and `modules/<id>/...` is its
    private tree. Everything else -- db1/, db2/, bare filenames -- is a lower
    layer, not a peer, and is none of this check's business.
    """
    parts = header.split("/")
    if len(parts) < 2 or parts[0] not in MODULE_ROOTS:
        return None
    return parts[1]


def crossings(root: Path):
    """Every (path, header) pair where a module includes a peer's header."""
    modules = root / MODULES
    if not modules.is_dir():
        raise CheckError(f"rule=module-root-missing path={MODULES}")

    found: set[tuple[str, str]] = set()
    for path in sorted((*modules.rglob("*.c"), *modules.rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        owner = owning_module(relative)
        for header in INCLUDE.findall(path.read_text(encoding="utf-8")):
            if header in BUS_TRANSPORT_HEADERS:
                continue
            peer = included_module(header)
            if peer is None or peer == owner or peer == CORE:
                continue
            found.add((relative, header))
    return found


def validate(root: Path) -> None:
    found = crossings(root)

    undeclared = sorted(found - ALLOWED)
    if undeclared:
        raise CheckError(
            "rule=undeclared-cross-module "
            f"crossings={[f'{path} -> {header}' for path, header in undeclared]} "
            "(a module may only reach a peer over the event bus)"
        )

    stale = sorted(ALLOWED - found)
    if stale:
        raise CheckError(
            "rule=stale-allowlist "
            f"crossings={[f'{path} -> {header}' for path, header in stale]} "
            "(coupling is gone; delete the entry so the list keeps shrinking)"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except (CheckError, OSError, UnicodeError) as exc:
        print(f"module-bus-boundary: ERROR {exc}", file=sys.stderr)
        return 1
    print(f"module-bus-boundary: ok ({len(ALLOWED)} declared crossings remain)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
