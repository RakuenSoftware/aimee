#!/usr/bin/env python3
"""Validate the proposal-local Git core contract and its approval evidence."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
from typing import NoReturn


DEFAULT_CONTRACT = Path("docs/proposals/done/git-core-contract.md")
DEFAULT_HANDOFF = Path("docs/validation/core-modularization-slice-2.md")
EVIDENCE_PATH = Path("docs/validation/roundtable/git-core-contract.json")
CUTOFF = "6ce37f53e1f627c19e15fc01f68959f546a5eded"
CONTRACT_VERSION = "1.0.0"
ID_RE = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
RUN_ID_RE = re.compile(r"^oprun_[A-Za-z0-9_]+$")
INVARIANT_IDS = (
    "git-required-core",
    "memory-owns-code-intelligence",
    "principal-scoped-ingest",
    "signed-producer-and-repository-provenance",
    "pre-persistence-secret-redaction",
)

EXPECTED_ACCEPTANCE = {
    "cross_principal_access": (
        "cross-principal-access", "integration", "deny", "PRINCIPAL_SCOPE_DENIED"
    ),
    "missing_principal_scope": (
        "missing-principal-scope", "integration", "deny", "PRINCIPAL_SCOPE_MISSING"
    ),
    "missing_producer_signature": (
        "missing-producer-signature", "integration", "deny", "PRODUCER_SIGNATURE_MISSING"
    ),
    "missing_repository_signature": (
        "missing-repository-signature", "integration", "deny", "REPOSITORY_SIGNATURE_MISSING"
    ),
    "producer_verifier_failure": (
        "producer-verifier-failure", "integration", "deny", "PRODUCER_VERIFICATION_FAILED"
    ),
    "repository_verifier_failure": (
        "repository-verifier-failure", "integration", "deny", "REPOSITORY_VERIFICATION_FAILED"
    ),
    "unknown_payload_class": (
        "unknown-payload-class", "integration", "deny", "UNKNOWN_PAYLOAD_CLASS"
    ),
    "incomplete_redaction_scan": (
        "incomplete-redaction-scan", "integration", "deny", "REDACTION_SCAN_INCOMPLETE"
    ),
    "redactor_error": ("redactor-error", "integration", "deny", "REDACTION_ENGINE_FAILED"),
    "unredacted_secret": ("unredacted-secret", "integration", "deny", "REDACTION_DENY"),
    "split_input_secret": ("split-input-secret", "integration", "deny", "REDACTION_DENY"),
    "partial_persistence_prevented": (
        "partial-persistence-prevented", "integration", "deny", "PARTIAL_PERSISTENCE_FORBIDDEN"
    ),
    "direct_git_persistence": (
        "direct-git-persistence", "mechanical", "deny", "GIT_PERSIST_FORBIDDEN"
    ),
    "git_owned_code_intelligence": (
        "git-owned-code-intelligence", "mechanical", "deny", "MEMORY_EXCLUSIVE"
    ),
    "submit_only_violation": (
        "submit-only-violation", "mechanical", "deny", "SUBMIT_ONLY_VIOLATION"
    ),
    "non_git_workspace_git_only_operation": (
        "non-git-workspace-git-only-operation",
        "integration",
        "capability_absent",
        "CAPABILITY_ABSENT",
    ),
    "signer_identity_reuse": (
        "signer-identity-reuse", "integration", "deny", "SIGNER_IDENTITY_REUSE"
    ),
    "deny_allow_overlap": ("deny-allow-overlap", "mechanical", "deny", "DENY_ALLOW_OVERLAP"),
    "post_approval_contract_mutation": (
        "post-approval-contract-mutation", "mechanical", "deny", "REVIEW_DIGEST_MISMATCH"
    ),
    "valid_repository_ingest": (
        "valid-repository-ingest", "integration", "pass", "CONTRACT_SATISFIED"
    ),
    "non_git_workspace_base_operation": (
        "non-git-workspace-base-operation", "integration", "pass", "CONTRACT_SATISFIED"
    ),
}

CANONICAL_NAMESPACE = "memory.code-intelligence.repository-records"
CANONICAL_INGEST = "memory.repository-record.ingest.v1"
EXPECTED_READ_SCOPES = {
    ("workspace.repository", "git", "read"),
    (CANONICAL_NAMESPACE, "memory", "read"),
}
EXPECTED_WRITE_SCOPES = {(CANONICAL_NAMESPACE, "memory", "write")}
EXPECTED_TRIGGERS = {
    "descriptors": {("src/modules/git/module.yaml", "descriptor")},
    "generated_builds": {
        ("src/generated/modules.mk", "make-build"),
        ("cmake/generated/modules.cmake", "cmake-build"),
    },
    "generated_profiles": {("build/inventory/core.json", "generated-profile")},
}
EXPECTED_READINESS = {("src/modules/git/module.yaml", "readiness", "ready")}
EXPECTED_STATUS_ROOTS = {
    ("docs/proposals", "git-runtime-ready"),
    ("docs/modules", "git-runtime-ready"),
}


class ContractError(ValueError):
    """A fail-closed, operator-readable contract error."""


def fail(rule: str, message: str, *, path: Path | str) -> NoReturn:
    raise ContractError(f"{path}: rule={rule}: {message}")


def _no_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"rule=json-duplicate-key: duplicate object key {key!r}")
        result[key] = value
    return result


def _reject_float(value: str) -> NoReturn:
    raise ContractError(f"rule=json-number-domain: floating/exponent number forbidden: {value}")


def _reject_constant(value: str) -> NoReturn:
    raise ContractError(f"rule=json-number-domain: non-finite constant forbidden: {value}")


def loads_strict(raw: str, *, path: Path | str) -> object:
    try:
        return json.loads(
            raw,
            object_pairs_hook=_no_duplicate_keys,
            parse_float=_reject_float,
            parse_constant=_reject_constant,
        )
    except ContractError as exc:
        raise ContractError(f"{path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{exc.msg} at {exc.lineno}:{exc.colno}", path=path)


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        fail("input", f"cannot read: {exc}", path=path)


def extract_contract(path: Path) -> dict[str, object]:
    return extract_json_fence(path, "git-core-contract")


def extract_json_fence(path: Path, name: str) -> dict[str, object]:
    raw = _read_text(path)
    opening = re.compile(rf"^```json {re.escape(name)}[ \t]*$", re.MULTILINE)
    matches = list(opening.finditer(raw))
    if len(matches) != 1:
        fail("contract-fence", f"expected exactly one {name} JSON fence", path=path)
    body = raw[matches[0].end() :]
    if body.startswith("\r\n"):
        body = body[2:]
    elif body.startswith("\n"):
        body = body[1:]
    closing = re.search(r"^```[ \t]*$", body, re.MULTILINE)
    if closing is None:
        fail("contract-fence", f"unterminated {name} JSON fence", path=path)
    payload = body[: closing.start()].rstrip("\r\n")
    value = loads_strict(payload, path=path)
    if not isinstance(value, dict):
        fail("schema", f"{name} must be a JSON object", path=path)
    return value


def validate_handoff(value: dict[str, object], path: Path) -> None:
    handoff = _object(
        value,
        {
            "schema_version",
            "receiver",
            "contract_file",
            "evidence_file",
            "invariants_source",
            "ordering_script_baseline",
            "trigger_surface_source",
        },
        "slice3-handoff",
        path,
    )
    _literal(handoff["schema_version"], 1, "handoff.schema_version", path)
    _literal(
        handoff["receiver"], "slice-3-proposal-ordering-gate", "handoff.receiver", path
    )
    _literal(
        handoff["contract_file"], DEFAULT_CONTRACT.as_posix(), "handoff.contract_file", path
    )
    _literal(
        handoff["evidence_file"], EVIDENCE_PATH.as_posix(), "handoff.evidence_file", path
    )
    _literal(
        handoff["invariants_source"],
        "git-core-contract.invariants",
        "handoff.invariants_source",
        path,
    )
    _literal(
        handoff["ordering_script_baseline"], CUTOFF, "handoff.ordering_script_baseline", path
    )
    _literal(
        handoff["trigger_surface_source"],
        "git-core-contract",
        "handoff.trigger_surface_source",
        path,
    )


def _object(value: object, keys: set[str], label: str, path: Path) -> dict[str, object]:
    if not isinstance(value, dict):
        fail("schema", f"{label} must be an object", path=path)
    actual = set(value)
    if actual != keys:
        fail(
            "schema",
            f"{label} keys mismatch; missing={sorted(keys-actual)}, unknown={sorted(actual-keys)}",
            path=path,
        )
    return value


def _list(value: object, label: str, path: Path, *, nonempty: bool = True) -> list[object]:
    if not isinstance(value, list):
        fail("schema", f"{label} must be an array", path=path)
    if nonempty and not value:
        fail("schema", f"{label} must not be empty", path=path)
    return value


def _string(value: object, label: str, path: Path, *, identifier: bool = False) -> str:
    if not isinstance(value, str) or not value:
        fail("schema", f"{label} must be a non-empty string", path=path)
    if identifier and not ID_RE.fullmatch(value):
        fail("identifier", f"{label} has invalid ID {value!r}", path=path)
    return value


def _literal(value: object, expected: object, label: str, path: Path) -> None:
    if type(value) is not type(expected) or value != expected:
        fail("pinned-value", f"{label} expected {expected!r}, actual {value!r}", path=path)


def _unique(values: list[object], key, label: str, path: Path) -> None:
    seen: set[object] = set()
    for value in values:
        item = key(value)
        if item in seen:
            fail("uniqueness", f"duplicate {label} {item!r}", path=path)
        seen.add(item)


def _safe_relative(value: object, label: str, path: Path) -> str:
    text = _string(value, label, path)
    if "\\" in text or "\x00" in text:
        fail("path", f"{label} contains a forbidden character", path=path)
    posix = PurePosixPath(text)
    if (
        posix.is_absolute()
        or any(part in {"", ".", ".."} for part in text.split("/"))
        or posix.as_posix() != text
    ):
        fail("path", f"{label} must be a normalized repository-relative path", path=path)
    return text


def canonical_review_bytes(contract: dict[str, object]) -> bytes:
    normalized = copy.deepcopy(contract)
    lifecycle = normalized.get("lifecycle")
    if not isinstance(lifecycle, dict):
        raise ContractError("rule=schema: lifecycle must be an object before digesting")
    lifecycle["status"] = "pending"
    lifecycle["approval_evidence"] = None
    return json.dumps(
        normalized, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def reviewed_contract_sha256(contract: dict[str, object]) -> str:
    return hashlib.sha256(canonical_review_bytes(contract)).hexdigest()


def _validate_scopes(principal: dict[str, object], path: Path) -> None:
    _literal(principal["policy"], "default-deny", "principal.policy", path)
    read_scope = _list(principal["read_scope"], "principal.read_scope", path)
    write_scope = _list(principal["write_scope"], "principal.write_scope", path)

    def validate(entries: list[object], label: str, expected_access: str) -> None:
        for index, raw in enumerate(entries):
            entry = _object(
                raw, {"namespace", "principal", "access"}, f"{label}[{index}]", path
            )
            _string(entry["namespace"], f"{label}[{index}].namespace", path, identifier=True)
            _string(entry["principal"], f"{label}[{index}].principal", path, identifier=True)
            _literal(entry["access"], expected_access, f"{label}[{index}].access", path)
        _unique(
            entries,
            lambda item: (item["namespace"], item["principal"], item["access"]),
            label,
            path,
        )

    validate(read_scope, "principal.read_scope", "read")
    validate(write_scope, "principal.write_scope", "write")
    actual_read = {
        (entry["namespace"], entry["principal"], entry["access"]) for entry in read_scope
    }
    actual_write = {
        (entry["namespace"], entry["principal"], entry["access"]) for entry in write_scope
    }
    if actual_read != EXPECTED_READ_SCOPES:
        fail("principal-scope", "read_scope differs from the v1 contract", path=path)
    if actual_write != EXPECTED_WRITE_SCOPES:
        fail("principal-scope", "write_scope differs from the v1 contract", path=path)
    namespaces = _list(
        principal["code_intelligence_namespaces"],
        "principal.code_intelligence_namespaces",
        path,
    )
    for index, namespace in enumerate(namespaces):
        _string(namespace, f"code_intelligence_namespaces[{index}]", path, identifier=True)
    _unique(namespaces, lambda value: value, "code-intelligence namespace", path)
    if namespaces != [CANONICAL_NAMESPACE]:
        fail(
            "memory-exclusive",
            f"code_intelligence_namespaces must equal {[CANONICAL_NAMESPACE]!r}",
            path=path,
        )
    for namespace in namespaces:
        writers = {
            entry["principal"]
            for entry in write_scope
            if entry["namespace"] == namespace and entry["access"] == "write"
        }
        if writers != {"memory"}:
            fail(
                "memory-exclusive",
                f"namespace {namespace!r} must have exactly memory as writer, "
                f"actual {sorted(writers)}",
                path=path,
            )


def _validate_provenance(value: object, path: Path) -> None:
    provenance = _object(
        value, {"producer", "repository", "require_both", "identity_rule"}, "provenance", path
    )
    verifier_by_side = {
        "producer": "git.provenance.producer.verify.v1",
        "repository": "git.provenance.repository.verify.v1",
    }
    for side in ("producer", "repository"):
        side_obj = _object(provenance[side], {"signature"}, f"provenance.{side}", path)
        signature = _object(
            side_obj["signature"],
            {"algorithms", "verifier_contract"},
            f"provenance.{side}.signature",
            path,
        )
        algorithms = _list(
            signature["algorithms"], f"provenance.{side}.signature.algorithms", path
        )
        for index, algorithm in enumerate(algorithms):
            _string(
                algorithm,
                f"provenance.{side}.signature.algorithms[{index}]",
                path,
                identifier=True,
            )
        _unique(algorithms, lambda item: item, f"{side} signature algorithm", path)
        if set(algorithms) != {"ssh-ed25519", "openpgp"}:
            fail("provenance", f"{side} signature algorithm allowlist differs from v1", path=path)
        _literal(
            signature["verifier_contract"],
            verifier_by_side[side],
            f"provenance.{side}.signature.verifier_contract",
            path,
        )
    _literal(provenance["require_both"], True, "provenance.require_both", path)
    _literal(
        provenance["identity_rule"],
        "distinct-key-identities",
        "provenance.identity_rule",
        path,
    )


def _validate_redaction(value: object, path: Path) -> None:
    redaction = _object(
        value,
        {"policy", "stage", "allow_classes", "deny_classes", "applies_to", "audit_contract"},
        "redaction",
        path,
    )
    _literal(redaction["policy"], "fail-closed", "redaction.policy", path)
    _literal(redaction["stage"], "before-memory-ingest", "redaction.stage", path)
    allowed_enum = {"source-code", "repository-metadata", "commit-metadata", "signature-metadata"}
    denied_enum = {"credentials", "tokens", "private-keys"}
    applies_enum = {"record-payload", "producer-attestation", "repository-provenance"}
    allow = _list(redaction["allow_classes"], "redaction.allow_classes", path)
    deny = _list(redaction["deny_classes"], "redaction.deny_classes", path)
    applies = _list(redaction["applies_to"], "redaction.applies_to", path)
    if set(allow) != allowed_enum:
        fail("redaction", f"allow_classes must equal {sorted(allowed_enum)}", path=path)
    if set(deny) != denied_enum:
        fail("redaction", f"deny_classes must equal {sorted(denied_enum)}", path=path)
    if set(applies) != applies_enum:
        fail("redaction", f"applies_to must equal {sorted(applies_enum)}", path=path)
    if set(allow) & set(deny):
        fail("redaction", "allow_classes and deny_classes overlap", path=path)
    for label, values in (("allow", allow), ("deny", deny), ("applies", applies)):
        if not all(isinstance(item, str) for item in values):
            fail("schema", f"redaction.{label} entries must be strings", path=path)
        _unique(values, lambda item: item, f"redaction {label} value", path)
    _string(redaction["audit_contract"], "redaction.audit_contract", path, identifier=True)


def _validate_triggers(value: object, path: Path) -> None:
    trigger = _object(
        value,
        {
            "descriptors",
            "generated_builds",
            "generated_profiles",
            "readiness_markers",
            "status_claim_roots",
        },
        "trigger_surface",
        path,
    )
    kind_by_group = {
        "descriptors": {"descriptor"},
        "generated_builds": {"make-build", "cmake-build"},
        "generated_profiles": {"generated-profile"},
    }
    for group, kinds in kind_by_group.items():
        entries = _list(trigger[group], f"trigger_surface.{group}", path)
        for index, raw in enumerate(entries):
            entry = _object(raw, {"path", "kind"}, f"{group}[{index}]", path)
            _safe_relative(entry["path"], f"{group}[{index}].path", path)
            if entry["kind"] not in kinds:
                fail("trigger-surface", f"{group}[{index}].kind is invalid", path=path)
        _unique(entries, lambda item: item["path"], f"{group} path", path)
        actual = {(entry["path"], entry["kind"]) for entry in entries}
        if actual != EXPECTED_TRIGGERS[group]:
            fail("trigger-surface", f"{group} differs from the v1 trigger set", path=path)
    readiness = _list(trigger["readiness_markers"], "trigger_surface.readiness_markers", path)
    for index, raw in enumerate(readiness):
        entry = _object(raw, {"path", "key", "value"}, f"readiness[{index}]", path)
        _safe_relative(entry["path"], f"readiness[{index}].path", path)
        _string(entry["key"], f"readiness[{index}].key", path, identifier=True)
        _literal(entry["value"], "ready", f"readiness[{index}].value", path)
    _unique(readiness, lambda item: item["path"], "readiness path", path)
    actual_readiness = {(item["path"], item["key"], item["value"]) for item in readiness}
    if actual_readiness != EXPECTED_READINESS:
        fail("trigger-surface", "readiness markers differ from the v1 trigger set", path=path)
    roots = _list(trigger["status_claim_roots"], "trigger_surface.status_claim_roots", path)
    for index, raw in enumerate(roots):
        entry = _object(raw, {"path", "claim"}, f"status root[{index}]", path)
        _safe_relative(entry["path"], f"status root[{index}].path", path)
        _literal(entry["claim"], "git-runtime-ready", f"status root[{index}].claim", path)
    _unique(roots, lambda item: item["path"], "status root path", path)
    actual_roots = {(item["path"], item["claim"]) for item in roots}
    if actual_roots != EXPECTED_STATUS_ROOTS:
        fail("trigger-surface", "status roots differ from the v1 trigger set", path=path)


def _validate_acceptance(value: object, path: Path) -> None:
    entries = _list(value, "acceptance", path)
    for index, raw in enumerate(entries):
        entry = _object(raw, {"id", "tier", "kind", "expected"}, f"acceptance[{index}]", path)
        _string(entry["id"], f"acceptance[{index}].id", path, identifier=True)
        kind = _string(entry["kind"], f"acceptance[{index}].kind", path)
        if kind not in EXPECTED_ACCEPTANCE:
            fail("acceptance", f"unknown acceptance kind {kind!r}", path=path)
        expected = _object(
            entry["expected"], {"decision", "reason_code"}, f"acceptance[{index}].expected", path
        )
        expected_id, tier, decision, reason = EXPECTED_ACCEPTANCE[kind]
        _literal(entry["id"], expected_id, f"acceptance[{index}].id", path)
        _literal(entry["tier"], tier, f"acceptance[{index}].tier", path)
        _literal(expected["decision"], decision, f"acceptance[{index}].expected.decision", path)
        _literal(
            expected["reason_code"], reason, f"acceptance[{index}].expected.reason_code", path
        )
    _unique(entries, lambda item: item["id"], "acceptance ID", path)
    _unique(entries, lambda item: item["kind"], "acceptance kind", path)
    actual = {item["kind"] for item in entries}
    if actual != set(EXPECTED_ACCEPTANCE):
        fail(
            "acceptance",
            f"acceptance kinds mismatch; missing={sorted(set(EXPECTED_ACCEPTANCE)-actual)}, "
            f"unknown={sorted(actual-set(EXPECTED_ACCEPTANCE))}",
            path=path,
        )


def _validate_evidence(
    lifecycle: dict[str, object], contract: dict[str, object], config_root: Path, path: Path
) -> None:
    approval = _object(
        lifecycle["approval_evidence"],
        {"run_id", "artifact_path", "file_sha256", "reviewed_contract_sha256"},
        "lifecycle.approval_evidence",
        path,
    )
    run_id = _string(approval["run_id"], "approval_evidence.run_id", path)
    if not RUN_ID_RE.fullmatch(run_id):
        fail("evidence", "approval_evidence.run_id is invalid", path=path)
    _literal(
        approval["artifact_path"], EVIDENCE_PATH.as_posix(), "approval_evidence.artifact_path", path
    )
    file_sha = _string(approval["file_sha256"], "approval_evidence.file_sha256", path)
    review_sha = _string(
        approval["reviewed_contract_sha256"], "approval_evidence.reviewed_contract_sha256", path
    )
    if not SHA_RE.fullmatch(file_sha) or not SHA_RE.fullmatch(review_sha):
        fail("evidence", "approval evidence digests must be lowercase SHA-256", path=path)

    evidence_path = resolve_under_root(config_root, EVIDENCE_PATH, label="evidence")
    raw = _read_text(evidence_path)
    try:
        actual_file_sha = hashlib.sha256(evidence_path.read_bytes()).hexdigest()
    except OSError as exc:
        fail("input", f"cannot hash evidence: {exc}", path=evidence_path)
    if actual_file_sha != file_sha:
        fail("evidence-file-digest", "evidence file SHA-256 mismatch", path=evidence_path)
    evidence = loads_strict(raw, path=evidence_path)
    evidence_obj = _object(
        evidence,
        {
            "schema_version",
            "run_id",
            "decision",
            "decided_at",
            "reviewed_contract_sha256",
            "findings",
            "overall",
        },
        "evidence",
        evidence_path,
    )
    _literal(evidence_obj["schema_version"], 1, "evidence.schema_version", evidence_path)
    _literal(evidence_obj["run_id"], run_id, "evidence.run_id", evidence_path)
    _literal(evidence_obj["decision"], "approve", "evidence.decision", evidence_path)
    decided_at = _string(evidence_obj["decided_at"], "evidence.decided_at", evidence_path)
    try:
        parsed = dt.datetime.fromisoformat(decided_at.replace("Z", "+00:00"))
    except ValueError as exc:
        fail("evidence", f"decided_at is not ISO-8601: {exc}", path=evidence_path)
    if parsed.tzinfo != dt.timezone.utc:
        fail("evidence", "decided_at must be UTC", path=evidence_path)
    _literal(
        evidence_obj["reviewed_contract_sha256"],
        review_sha,
        "evidence.reviewed_contract_sha256",
        evidence_path,
    )
    findings = _list(evidence_obj["findings"], "evidence.findings", evidence_path, nonempty=False)
    for index, raw_finding in enumerate(findings):
        finding = _object(
            raw_finding,
            {"severity", "location", "message"},
            f"evidence.findings[{index}]",
            evidence_path,
        )
        if finding["severity"] not in {"blocking", "non-blocking"}:
            fail("evidence", f"finding {index} severity is invalid", path=evidence_path)
        _string(finding["location"], f"finding {index} location", evidence_path)
        _string(finding["message"], f"finding {index} message", evidence_path)
        if finding["severity"] == "blocking":
            fail(
                "evidence-blocker",
                f"approval evidence contains blocking finding {index}",
                path=evidence_path,
            )
    _string(evidence_obj["overall"], "evidence.overall", evidence_path)
    actual_review_sha = reviewed_contract_sha256(contract)
    if actual_review_sha != review_sha:
        fail("review-digest", "substantive contract differs from approved content", path=path)


def _validate_git_cutoff(config_root: Path, cutoff: dict[str, object], path: Path) -> None:
    git_path = shutil.which("git", path="/usr/bin:/bin")
    if git_path != "/usr/bin/git":
        fail("git-cutoff", f"expected trusted git at /usr/bin/git, actual {git_path!r}", path=path)

    def run(*args: str) -> str:
        try:
            result = subprocess.run(
                [git_path, *args],
                cwd=config_root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        except OSError as exc:
            fail("git-cutoff", f"cannot execute git: {exc}", path=path)
        if result.returncode != 0:
            fail(
                "git-cutoff",
                f"git {' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}",
                path=path,
            )
        return result.stdout.strip()

    commit = str(cutoff["commit"])
    resolved = run("rev-parse", "--verify", f"{commit}^{{commit}}")
    if resolved != commit:
        fail("git-cutoff", f"cutoff resolved to {resolved!r}, expected {commit}", path=path)
    head = run("rev-parse", "HEAD")
    run("merge-base", "--is-ancestor", commit, head)
    tree_path = run("ls-tree", "-d", "--name-only", commit, "--", "src/modules/git")
    if tree_path != "src/modules/git":
        fail("git-cutoff", "src/modules/git is absent at historical cutoff", path=path)


def validate_contract(
    contract: dict[str, object],
    *,
    path: Path,
    config_root: Path,
    require_status: str,
    check_git: bool = True,
) -> None:
    top = _object(
        contract,
        {
            "schema_version",
            "contract_version",
            "module",
            "classification",
            "invariants",
            "lifecycle",
            "historical_cutoff", "ownership", "ingest_boundary", "principal", "provenance",
            "redaction", "non_git_workspace", "compatibility", "trigger_surface", "acceptance",
        },
        "contract",
        path,
    )
    _literal(top["schema_version"], 1, "schema_version", path)
    _literal(top["contract_version"], CONTRACT_VERSION, "contract_version", path)
    _literal(top["module"], "git", "module", path)
    _literal(top["classification"], "required", "classification", path)
    invariants = _list(top["invariants"], "invariants", path)
    if tuple(invariants) != INVARIANT_IDS:
        fail(
            "contract-invariants",
            f"expected ordered invariants {list(INVARIANT_IDS)!r}, actual {invariants!r}",
            path=path,
        )

    lifecycle = _object(
        top["lifecycle"], {"status", "enforcement_scope", "approval_evidence"}, "lifecycle", path
    )
    _literal(lifecycle["status"], require_status, "lifecycle.status", path)
    _literal(lifecycle["enforcement_scope"], "structural-only", "lifecycle.enforcement_scope", path)
    if require_status == "pending":
        if lifecycle["approval_evidence"] is not None:
            fail("lifecycle", "pending contract must have null approval evidence", path=path)
        evidence = resolve_under_root(config_root, EVIDENCE_PATH, label="evidence")
        if evidence.exists():
            fail(
                "lifecycle",
                "pending contract must not have an approval evidence file",
                path=evidence,
            )
    elif require_status == "roundtable-approved":
        _validate_evidence(lifecycle, contract, config_root, path)
    else:
        fail("lifecycle", f"unsupported required status {require_status!r}", path=path)

    cutoff = _object(
        top["historical_cutoff"],
        {"commit", "ref", "pinned_after_slice", "path"},
        "historical_cutoff",
        path,
    )
    _literal(cutoff["commit"], CUTOFF, "historical_cutoff.commit", path)
    _literal(cutoff["ref"], "refs/heads/feature/core-modularization", "historical_cutoff.ref", path)
    _literal(cutoff["pinned_after_slice"], 1, "historical_cutoff.pinned_after_slice", path)
    _literal(cutoff["path"], "src/modules/git", "historical_cutoff.path", path)

    ownership = _object(
        top["ownership"],
        {
            "producer",
            "memory_owner",
            "code_intelligence_exclusive_to_memory",
            "git_may_persist_code_intelligence",
        },
        "ownership",
        path,
    )
    _literal(ownership["producer"], "git", "ownership.producer", path)
    _literal(ownership["memory_owner"], "memory", "ownership.memory_owner", path)
    _literal(
        ownership["code_intelligence_exclusive_to_memory"],
        True,
        "ownership.code_intelligence_exclusive_to_memory",
        path,
    )
    _literal(
        ownership["git_may_persist_code_intelligence"],
        False,
        "ownership.git_may_persist_code_intelligence",
        path,
    )

    ingest = _object(
        top["ingest_boundary"],
        {"contract_id", "namespace", "writer", "git_access"},
        "ingest_boundary",
        path,
    )
    _literal(ingest["contract_id"], CANONICAL_INGEST, "ingest_boundary.contract_id", path)
    _literal(ingest["namespace"], CANONICAL_NAMESPACE, "ingest_boundary.namespace", path)
    _literal(ingest["writer"], "memory", "ingest_boundary.writer", path)
    _literal(ingest["git_access"], "submit-only", "ingest_boundary.git_access", path)

    principal = _object(
        top["principal"],
        {"policy", "read_scope", "write_scope", "code_intelligence_namespaces"},
        "principal",
        path,
    )
    _validate_scopes(principal, path)
    if ingest["namespace"] not in principal["code_intelligence_namespaces"]:
        fail("ingest-boundary", "ingest namespace is not code-intelligence-owned", path=path)

    _validate_provenance(top["provenance"], path)
    _validate_redaction(top["redaction"], path)
    non_git = _object(
        top["non_git_workspace"],
        {"behavior", "git_only_operation"},
        "non_git_workspace",
        path,
    )
    _literal(non_git["behavior"], "usable", "non_git_workspace.behavior", path)
    _literal(
        non_git["git_only_operation"],
        "capability_absent",
        "non_git_workspace.git_only_operation",
        path,
    )

    compatibility = _object(top["compatibility"], {"legacy_entrypoints"}, "compatibility", path)
    aliases = _list(
        compatibility["legacy_entrypoints"],
        "compatibility.legacy_entrypoints",
        path,
        nonempty=False,
    )
    if aliases:
        fail("compatibility", "contract version 1.0.0 permits no legacy entrypoints", path=path)
    for index, raw in enumerate(aliases):
        alias = _object(
            raw,
            {"surface", "entrypoint", "canonical_contract", "expires"},
            f"legacy alias[{index}]",
            path,
        )
        _string(alias["surface"], f"legacy alias[{index}].surface", path, identifier=True)
        _string(alias["entrypoint"], f"legacy alias[{index}].entrypoint", path)
        _string(
            alias["canonical_contract"],
            f"legacy alias[{index}].canonical_contract",
            path,
            identifier=True,
        )
        try:
            expiry = dt.date.fromisoformat(
                _string(alias["expires"], f"legacy alias[{index}].expires", path)
            )
        except ValueError as exc:
            fail("compatibility", f"legacy alias[{index}] expiry invalid: {exc}", path=path)
        if expiry < dt.datetime.now(dt.timezone.utc).date():
            fail("compatibility", f"legacy alias[{index}] is expired", path=path)
    _unique(aliases, lambda item: item["surface"], "legacy surface", path)

    _validate_triggers(top["trigger_surface"], path)
    _validate_acceptance(top["acceptance"], path)
    if check_git:
        _validate_git_cutoff(config_root, cutoff, path)


def resolve_under_root(root: Path, value: Path, *, label: str) -> Path:
    candidate = value if value.is_absolute() else root / value
    resolved = Path(os.path.realpath(candidate))
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        fail("path-containment", f"{label} escapes config root {root}", path=resolved)
    return resolved


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path)
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    parser.add_argument(
        "--require-status", choices=("pending", "roundtable-approved"), required=True
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    script_root = Path(__file__).resolve().parent.parent
    root_value = args.config_root or os.environ.get("AIMEE_CONFIG_ROOT") or script_root
    config_root = Path(os.path.realpath(root_value))
    if not config_root.is_dir():
        print(
            f"check_git_core_contract: error: rule=config-root: {config_root} is not a directory",
            file=sys.stderr,
        )
        return 1
    try:
        contract_path = resolve_under_root(config_root, args.contract, label="contract")
        contract = extract_contract(contract_path)
        handoff_path = resolve_under_root(config_root, DEFAULT_HANDOFF, label="handoff")
        validate_handoff(extract_json_fence(handoff_path, "slice3-handoff"), handoff_path)
        validate_contract(
            contract,
            path=contract_path,
            config_root=config_root,
            require_status=args.require_status,
            check_git=True,
        )
    except ContractError as exc:
        print(f"check_git_core_contract: error: {exc}", file=sys.stderr)
        return 1
    print(f"check_git_core_contract: ok ({args.require_status}; {contract_path})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
