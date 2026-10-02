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
BUS_TRANSPORT_HEADERS = {"aimee/audit/obs_bus.h", "aimee/postgres/client.h"}
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
    ("src/modules/translation/include/aimee/translation/aimee_backend.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/translation/include/aimee/translation/aimee_frontend.h", "aimee/ir/aimee_ir.h"),
    ("src/modules/translation/include/aimee/translation/aimee_ir_stream.h", "aimee/ir/aimee_ir.h"),
}
# A peer service called directly in process. Each one must become bus traffic.
PENDING_BUS_MIGRATION = {
    ("src/modules/delegates/delegate_openai.c", "aimee/tools/agent_tools.h"),
    ("src/modules/delegates/delegate_run_phases.c", "aimee/workspace/workspace.h"),
    ("src/modules/guardrails/guardrails_action_audit.c", "aimee/audit/audit_action.h"),
    ("src/modules/guardrails/guardrails_action_audit.c", "aimee/audit/audit_worm.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "aimee/skills/skill.h"),
    ("src/modules/roadmap/roadmap_auto.c", "aimee/delegates/delegate_launch.h"),
    ("src/modules/roundtable/delegate_ensemble.c", "aimee/delegates/delegate_credentials.h"),
    ("src/modules/tools/agent_tools.c", "aimee/protocols/mcp/mcp_client_registry.h"),
    ("src/modules/tools/agent_tools.c", "aimee/workspace/workspace.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "aimee/protocols/mcp/mcp_client_registry.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "aimee/workspace/workspace.h"),
    ("src/modules/workspace/workspace_provider_container.h", "aimee/delegates/delegate_backend.h"),
}
# Worse than the above: these reach past a peer's public API into its private
# headers, so the coupling is not even to a published contract. Retiring one
# means the owner grows a bus stage for what the caller needs, not that the
# caller switches to the peer's public header.
PRIVATE_HEADER_REACH = {
    # DB1 moved under src/modules, so every pre-existing reach into it became a
    # visible crossing. These carry a modules/ prefix. Phase B of the DB1 proposal
    # retires them, and stale-allowlist forces each line out as it goes -- this
    # block emptying IS that migration's progress bar.
    ("src/modules/git/git_ops.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/git/git_project.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/git/mcp_git_query.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/guardrails/guardrails.c", "modules/git/git_verify.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/git/git_verify.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "modules/workspace/workspace_turn.h"),
    ("src/modules/guardrails/guardrails_tdd.c", "modules/git/git_verify.h"),
    ("src/modules/tools/agent_tools.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/tools/agent_tools_anchored.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/tools/agent_tools_effect_contract.c", "modules/workspace/workspace_provider.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/git/forge_credentials.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/git/git_cred_inject.h"),
    ("src/modules/webuser/webuser_editor.c", "modules/workspace/workspace_scope.h"),
    ("src/modules/workflows/wfe_live_forge.c", "modules/git/git_cred_inject.h"),
    ("src/modules/workflows/wfe_live_forge.c", "modules/git/git_pr_api.h"),
    ("src/modules/workspace/workspace_turn.c", "modules/git/forge_credentials.h"),
    ("src/modules/workspace/workspace_turn.c", "modules/git/git_cred_inject.h"),
}
# Reaches into a module the build links into core rather than supervising as a bus
# peer (`execution: core` in the repository lock): config, vault, audit,
# execution-policy, gateway, ir, module-runtime, protocols, translation.
#
# These are the invariant's real exception, and the point of listing them is that
# it stops being unstated. A credential store cannot be a bus peer -- git_cred_inject
# promises its token is "never logged" while the bus keeps an ordered capture and
# audit tap -- so vault is core by necessity, not by omission. Whether every module
# here belongs in core is a question this list is meant to make askable.
CORE_LINKED_REACH = {
    # db1 reaching config, which is core-linked. Retires when db1 stops reading
    # config directly.
    ("src/modules/delegates/delegate_credential_retry.c", "vault/runtime_secret.h"),
    ("src/modules/delegates/delegate_credential_retry.c", "vault/vault_service.h"),
    ("src/modules/delegates/include/aimee/delegates/delegate_credentials.h", "vault/vault_principal.h"),
    ("src/modules/git/git_forge_vault.c", "vault/vault_service.h"),
    ("src/modules/git/git_host_cred.c", "vault/vault_service.h"),
    ("src/modules/git/git_host_cred.c", "vault/vault_store.h"),
    ("src/modules/git/git_oauth_device.c", "vault/vault_service.h"),
    ("src/modules/git/git_oauth_github.c", "vault/runtime_secret.h"),
    ("src/modules/git/git_oauth_github.c", "vault/vault_service.h"),
    ("src/modules/kb_client/kb_client.c", "vault/runtime_secret.h"),
    ("src/modules/kb_client/kb_client_mtls.c", "vault/runtime_secret.h"),
    ("src/modules/kb_client/kb_client_ws.c", "vault/runtime_secret.h"),
    ("src/modules/protocols/mcp/mcp_client_registry.c", "vault/runtime_secret.h"),
    ("src/modules/roundtable/delegate_ensemble.c", "vault/runtime_secret.h"),
}
# Reaches into a module that is NOT core-linked, through the flat include root the
# build puts on the path. Same coupling as PRIVATE_HEADER_REACH, reached by bare
# filename instead of a prefix, and invisible until this check learned to resolve
# names. Retiring one means the owner grows a bus stage, or the caller stops being
# a separate module.
FLAT_ROOT_REACH = {
    # The same DB1 move, reached by bare filename because the build puts
    # -Imodules/db1 on the include path. Two run the other way, db1 itself
    # reaching config, and retire when db1 stops linking config directly.
    ("src/modules/delegates/delegate_prompt.c", "kb_client/kb_client.h"),
    ("src/modules/delegates/delegate_run_phases.c", "guardrails/guardrails.h"),
    ("src/modules/delegates/delegate_sandbox_image.c", "guardrails/guardrails.h"),
    ("src/modules/delegates/delegate_sandbox_image.c", "sandbox/sandbox_learned.h"),
    ("src/modules/delegates/include/aimee/delegates/panel_provider.h", "roundtable/roundtable_types.h"),
    ("src/modules/git/git_ssh_agent.c", "webuser/webuser_runtime.h"),
    ("src/modules/git/git_verify_ops.c", "guardrails/guardrails.h"),
    ("src/modules/git/mcp_git_pr.c", "guardrails/guardrails.h"),
    ("src/modules/git/mcp_git_query.c", "guardrails/guardrails.h"),
    ("src/modules/git/mcp_git_write.c", "guardrails/guardrails.h"),
    ("src/modules/guardrails/guardrails.c", "kb_client/kb_client.h"),
    ("src/modules/guardrails/guardrails_blast_radius.c", "kb_client/kb_client.h"),
    ("src/modules/guardrails/guardrails_orchestrator.c", "kb_client/kb_client.h"),
    ("src/modules/sandbox/sandbox_learned.c", "guardrails/guardrails.h"),
    ("src/modules/tools/agent_tools.c", "economizer/economizer.h"),
    ("src/modules/tools/agent_tools.c", "kb_client/kb_client.h"),
    ("src/modules/tools/agent_tools.c", "lsp/lsp.h"),
    ("src/modules/tools/agent_tools.c", "sandbox/sandbox_learned.h"),
    ("src/modules/tools/agent_tools_anchored.c", "economizer/economizer.h"),
    ("src/modules/tools/agent_tools_anchored.c", "guardrails/guardrails.h"),
    ("src/modules/tools/agent_tools_anchored.c", "kb_client/kb_client.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "economizer/economizer.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "guardrails/guardrails_blast_radius.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "kb_client/kb_client.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "lsp/lsp.h"),
    ("src/modules/tools/agent_tools_dispatch.c", "sandbox/sandbox_learned.h"),
    ("src/modules/workflows/wfe_live_panel.c", "roundtable/roundtable_verify.h"),
    ("src/modules/workflows/wfe_panel_roundtable.h", "roundtable/delegate_ensemble.h"),
    ("src/modules/workspace/workspace.c", "kb_client/kb_client.h"),
}
# Native knowledge algorithms were already linked by the KB host. Their exact
# existing header edges moved with ownership from the retired KB_STORE provider.
# This closed set replaces the former blanket KB_STORE C-tree exemption; adding a
# new peer include or retaining a removed edge now fails this checker.
KNOWLEDGE_HOST_REACH = {
    ('src/modules/guardrails/guardrails_semantic.c', 'modules/kb/c/bandit.h'),
    ('src/modules/kb-synthesis/kb_curator_contradictions.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_contradictions.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_contradictions.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/cross_repo_build.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/cross_repo_identity.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/cross_repo_route.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/cross_repo_stats.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/kb_store.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/kb_store_tenant.h'),
    ('src/modules/kb-synthesis/kb_curator_drain.c', 'modules/kb/c/decision_log.h'),
    ('src/modules/kb-synthesis/kb_curator_extract.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_extract.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_extract.c', 'modules/kb/c/kb_store_tenant.h'),
    ('src/modules/kb-synthesis/kb_curator_extract.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_extract.c', 'modules/kb/c/feature_rows.h'),
    ('src/modules/kb-synthesis/kb_curator_extract_code.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_extract_code.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_extract_code.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_index_claims.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_index_claims.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_index_claims.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_index_claims.c', 'modules/kb/c/pgvec_transport.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/kb_payload.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/kb_runtime_state.h'),
    ('src/modules/kb-synthesis/kb_curator_index_code_unit.c', 'modules/kb/c/pgvec_transport.h'),
    ('src/modules/kb-synthesis/kb_curator_index_narrative.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_index_narrative.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_index_narrative.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_index_narrative.c', 'modules/kb/c/pgvec_transport.h'),
    ('src/modules/kb-synthesis/kb_curator_link_artifacts.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_link_artifacts.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_link_artifacts.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_link_artifacts.c', 'modules/kb/c/pgvec_transport.h'),
    ('src/modules/kb-synthesis/kb_curator_promote.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_promote.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_promote.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_queue.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_queue.c', 'modules/kb/c/kb_store_tenant.h'),
    ('src/modules/kb-synthesis/kb_curator_queue.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_queue.c', 'modules/kb/c/kb_payload.h'),
    ('src/modules/kb-synthesis/kb_curator_resolve_entities.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_resolve_entities.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_resolve_entities.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_resolve_entities.c', 'modules/kb/c/pgvec_transport.h'),
    ('src/modules/kb-synthesis/kb_curator_serve.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_serve.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_synthesize.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_synthesize.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/kb-synthesis/kb_curator_synthesize.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/kb-synthesis/kb_curator_version.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/kb-synthesis/kb_curator_version.c', 'modules/kb/c/kb_payload.h'),
    ('src/modules/kb-synthesis/kb_curator_version.c', 'modules/kb/c/kb_runtime_state.h'),
    ('src/modules/kb/c/canonical_index.h', 'css/css_analyze.h'),
    ('src/modules/kb/c/css_graph.h', 'css/css_analyze.h'),
    ('src/modules/kb/c/kb_store_learning.h', 'aimee/learning/learning.h'),
    ('src/modules/kb/c/kb_store_vault_witness_provider.h', 'modules/vault/vault_witness_checkpoint.h'),
    ('src/modules/kb/c/kb_store_vault_witness_provider.h', 'modules/vault/vault_witness_merkle.h'),
    ('src/modules/kb/c/kb_store_vault_witness_provider.h', 'modules/vault/vault_witness_record.h'),
    ('src/modules/kb/c/kb_store_witness_checkpoint.c', 'modules/vault/vault_witness_checkpoint.h'),
    ('src/modules/kb/c/kb_store_witness_checkpoint.c', 'modules/vault/vault_witness_export.h'),
    ('src/modules/kb/c/kb_store_witness_checkpoint.c', 'modules/vault/vault_witness_merkle.h'),
    ('src/modules/kb/c/kb_store_witness_checkpoint.c', 'modules/vault/vault_witness_record.h'),
    ('src/modules/kb/c/kb_store_witness_emit.c', 'modules/vault/vault_witness_checkpoint.h'),
    ('src/modules/kb/c/kb_store_witness_emit.c', 'modules/vault/vault_witness_record.h'),
    ('src/modules/kb/c/kb_store_witness_emit.h', 'modules/vault/vault_witness_export.h'),
    ('src/modules/kb/c/kb_audit_worm.c', 'aimee/audit/audit_worm_chain.h'),
    ('src/modules/kb/c/org_vault_rewrap.h', 'vault/vault_crypto.h'),
    ('src/modules/kb/c/org_vault_rewrap.h', 'vault/vault_reseal_receipt.h'),
    ('src/modules/kb/c/vault_pg.c', 'vault/vault_crypto.h'),
    ('src/modules/kb/c/vault_pg.c', 'vault/vault_principal.h'),
    ('src/modules/kb/c/vault_pg.c', 'vault/vault_store.h'),
    ('src/modules/kb/c/vault_pg.h', 'vault/vault_internal.h'),
    ('src/modules/learning/learning_bundle.c', 'modules/kb/c/evidence_vectors.h'),
    ('src/modules/learning/learning_endogeneity.c', 'modules/kb/c/kb_store_learning.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/anti_patterns.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/demotion.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/entity_nodes.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/evidence_vectors.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/kb_service_backend.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/learning_synth_ops.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/rules.h'),
    ('src/modules/learning/learning_evidence.c', 'modules/kb/c/workflow_patterns.h'),
    ('src/modules/learning/learning_regret.c', 'modules/kb/c/kb_store_learning.h'),
    ('src/modules/learning/learning_router.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/learning/learning_router.c', 'modules/kb/c/collab_rules.h'),
    ('src/modules/learning/learning_router.c', 'modules/kb/c/kb_store_learning.h'),
    ('src/modules/roadmap/roadmap_auto.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/roadmap/roadmap_auto.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/roadmap/roadmap_auto.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/roadmap/roadmap_milestone.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/roadmap/roadmap_milestone.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/roadmap/roadmap_milestone.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/roadmap/roadmap_reassess.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/roadmap/roadmap_reassess.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/roadmap/roadmap_reassess.c', 'modules/kb/c/db_postgres.h'),
    ('src/modules/roadmap/roadmap_report.c', 'modules/kb/c/artifacts.h'),
    ('src/modules/roadmap/roadmap_report.c', 'modules/kb/c/kb_store_internal.h'),
    ('src/modules/roadmap/roadmap_report.c', 'modules/kb/c/db_postgres.h'),
}
ALLOWED = (IR_SHARED_TYPE | PENDING_BUS_MIGRATION | PRIVATE_HEADER_REACH |
           CORE_LINKED_REACH | FLAT_ROOT_REACH | KNOWLEDGE_HOST_REACH)
# Both bracket styles: a quoted include couples exactly as hard as an angled one,
# and the tree uses quoted form for every `modules/` reach and for three
# `aimee/protocols/` ones.
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


class CheckError(ValueError):
    """A module reached a peer module's header instead of the event bus."""


def owning_module(relative: str) -> str:
    """The module a source path belongs to, by its position under src/modules."""
    return relative.split("/")[2]


def flat_root_owners(root: Path) -> dict[str, str]:
    """Header basename -> the module whose directory root publishes it.

    The build puts most module directories on the include path (`-Imodules/<id>`
    or `-iquote modules/<id>`), so `#include "vault_service.h"` reaches straight
    into vault. Counting only `aimee/` and `modules/` prefixes missed all of it:
    122 crossings against the 45 those prefixes see.

    Ownership is by basename, which is only sound while basenames are unique. A
    collision, or a name that `src/headers` would resolve first, makes the answer
    depend on include order rather than on the name, so it is rejected outright
    rather than guessed at.
    """
    owners: dict[str, str] = {}
    collisions: list[str] = []
    modules = root / MODULES
    for module_dir in sorted(path for path in modules.iterdir() if path.is_dir()):
        for header in sorted(module_dir.glob("*.h")):
            previous = owners.get(header.name)
            if previous is not None:
                collisions.append(f"{header.name} in {previous} and {module_dir.name}")
                continue
            owners[header.name] = module_dir.name
    if collisions:
        raise CheckError(
            f"rule=ambiguous-module-header headers={sorted(collisions)} "
            "(a bare include would resolve by search order, not by owner)"
        )
    shadowed = sorted(name for name in owners if (root / "src/headers" / name).exists())
    if shadowed:
        raise CheckError(
            f"rule=shadowed-module-header headers={shadowed} "
            "(src/headers resolves first, so the owner cannot be read off the name)"
        )
    return owners


def included_module(header: str) -> str | None:
    """The module a prefixed include names, or None when it names no module.

    `aimee/<id>/...` is a module's public API and `modules/<id>/...` is its
    private tree. Everything else with a path -- db1/, kb_store/ -- is a lower layer,
    not a peer. Bare filenames are resolved separately, by owner lookup.
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

    owners = flat_root_owners(root)
    found: set[tuple[str, str]] = set()
    for path in sorted((*modules.rglob("*.c"), *modules.rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        owner = owning_module(relative)
        for header in INCLUDE.findall(path.read_text(encoding="utf-8")):
            if "/" not in header:
                # A bare include. It reaches a peer only when some module root
                # publishes the name and the including file's own directory does
                # not -- an own-directory header always wins for a quoted include.
                peer = owners.get(header)
                if peer is not None and peer != owner and not (path.parent / header).exists():
                    found.add((relative, f"{peer}/{header}"))
                continue
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
