#!/usr/bin/env python3
"""Deterministic contracts for module documentation attestation.

This module deliberately contains no identity-provider constants.  Deployments
provide a strict CI OIDC issuer profile; the verifier normalizes its claims into
the provider-independent workload record defined here.
"""

from __future__ import annotations

import base64
import binascii
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import struct
import tempfile
from typing import Callable, NamedTuple, NoReturn


MODULE_ID_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
HEX_64_RE = re.compile(r"^[0-9a-f]{64}$")
IDENTITY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@/-]{0,254}$")
CLAIM_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_.:/-]{0,127}$")
PROSE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ,.;:()'/_-]*[A-Za-z0-9.)]$")
TOKEN_RE = re.compile(r"[A-Za-z0-9]+(?:['-][A-Za-z0-9]+)*")
REFERENCE_RE = re.compile(
    r"^(?:module:(?P<module>[a-z][a-z0-9]*(?:-[a-z0-9]+)*)|"
    r"path:(?P<path>[A-Za-z0-9][A-Za-z0-9._/-]*)(?:#L(?P<line>[1-9][0-9]*))?)$"
)
TIMESTAMP_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")

SECTIONS = (
    "Purpose and non-goals",
    "Classification and lifecycle",
    "Public contracts",
    "Dependencies and consumers",
    "Providers and readiness",
    "Configuration and activation",
    "Routes, commands, protocols, and stages",
    "Data and migrations",
    "Security and privacy",
    "Supported journeys",
    "Tests and failure behavior",
    "Operational diagnostics",
    "Compatibility aliases",
    "Extension and removal rules",
)

WORKLOAD_FIELDS = (
    "repository_identity",
    "workflow_identity",
    "workflow_revision",
    "run_identity",
    "attempt",
    "trigger_check_identity",
    "event_type",
    "actor_identity",
    "runner_class",
)
SOURCE_PATTERN_RE = re.compile(
    r"^src/modules/(?P<module>[a-z][a-z0-9]*(?:-[a-z0-9]+)*)/"
    r"(?:(?P<literal>[a-z][a-z0-9_-]*)/)?\*\.(?P<suffix>[ch])$"
)


class ContractError(ValueError):
    """A fail-closed contract violation with a stable rule identifier."""


def fail(rule: str, message: str, *, line: int | None = None) -> NoReturn:
    location = f" line={line}" if line is not None else ""
    raise ContractError(f"rule={rule}{location}: {message}")


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            fail("json-duplicate-key", f"duplicate key {key!r}")
        result[key] = value
    return result


def _reject_number(value: str) -> NoReturn:
    fail("json-number-domain", f"forbidden number {value!r}")


def strict_json_bytes(raw: bytes) -> object:
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", "UTF-8 BOM is forbidden")
    try:
        text = raw.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        fail("json-encoding", str(exc))
    try:
        value = json.loads(
            text,
            object_pairs_hook=_object_without_duplicates,
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{exc.msg} at {exc.lineno}:{exc.colno}")
    _reject_surrogates(value)
    return value


def _reject_surrogates(value: object) -> None:
    if isinstance(value, str):
        if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
            fail("json-surrogate", "surrogate code point is forbidden")
    elif isinstance(value, list):
        for item in value:
            _reject_surrogates(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            _reject_surrogates(key)
            _reject_surrogates(item)


def _exact_object(value: object, keys: set[str], rule: str) -> dict[str, object]:
    if not isinstance(value, dict):
        fail(rule, "expected object")
    if set(value) != keys:
        fail(rule, f"keys mismatch; missing={sorted(keys-set(value))}, unknown={sorted(set(value)-keys)}")
    return value


def _string(value: object, rule: str) -> str:
    if not isinstance(value, str) or not value:
        fail(rule, "expected non-empty string")
    return value


def validate_oidc_profile(value: object) -> dict[str, object]:
    """Validate a provider-neutral CI issuer profile.

    Cryptographic JWT verification is performed by the deployed verifier.  This
    function closes the configuration surface that selects trust, algorithms,
    normalized claims, and immutable workload predicates.
    """
    profile = _exact_object(
        value,
        {
            "schema", "name", "issuer", "audience", "jwks", "allowed_algorithms",
            "max_token_lifetime_seconds", "clock_skew_seconds", "claim_mappings",
            "predicates", "repository_api",
        },
        "oidc-profile-shape",
    )
    if profile["schema"] != "aimee.ci-oidc-profile.v1":
        fail("oidc-profile-schema", "unsupported schema")
    name = _string(profile["name"], "oidc-profile-name")
    if not MODULE_ID_RE.fullmatch(name):
        fail("oidc-profile-name", "name must use canonical identifier syntax")
    for field in ("issuer", "audience"):
        text = _string(profile[field], f"oidc-{field}")
        if field == "issuer" and (not text.startswith("https://") or text.endswith("/")):
            fail("oidc-issuer", "issuer must be an exact HTTPS URL without trailing slash")

    jwks = _exact_object(
        profile["jwks"],
        {"uri", "tls_spki_sha256", "key_thumbprints_sha256", "max_age_seconds"},
        "oidc-jwks-shape",
    )
    if not _string(jwks["uri"], "oidc-jwks-uri").startswith("https://"):
        fail("oidc-jwks-uri", "JWKS URI must use HTTPS")
    pins = jwks["tls_spki_sha256"]
    if not isinstance(pins, list) or not pins or not all(isinstance(pin, str) and HEX_64_RE.fullmatch(pin) for pin in pins):
        fail("oidc-jwks-pins", "at least one lowercase SHA-256 SPKI pin is required")
    if pins != sorted(set(pins)):
        fail("oidc-jwks-pins", "SPKI pins must be sorted and unique")
    thumbprints = jwks["key_thumbprints_sha256"]
    if not isinstance(thumbprints, list) or not thumbprints or not all(
        isinstance(item, str) and HEX_64_RE.fullmatch(item) for item in thumbprints
    ):
        fail("oidc-jwks-keys", "at least one lowercase SHA-256 JWK thumbprint is required")
    if thumbprints != sorted(set(thumbprints)):
        fail("oidc-jwks-keys", "JWK thumbprints must be sorted and unique")
    if type(jwks["max_age_seconds"]) is not int or not 1 <= jwks["max_age_seconds"] <= 86400:
        fail("oidc-jwks-max-age", "max_age_seconds must be an integer in [1, 86400]")

    algorithms = profile["allowed_algorithms"]
    allowed = {"EdDSA", "ES256", "RS256", "PS256"}
    if not isinstance(algorithms, list) or not algorithms or algorithms != sorted(set(algorithms)):
        fail("oidc-algorithms", "allowed_algorithms must be a sorted unique non-empty array")
    if any(type(item) is not str or item not in allowed for item in algorithms):
        fail("oidc-algorithms", "algorithm is not in the closed allowlist")
    lifetime = profile["max_token_lifetime_seconds"]
    skew = profile["clock_skew_seconds"]
    if type(lifetime) is not int or not 1 <= lifetime <= 600:
        fail("oidc-token-lifetime", "maximum token lifetime must be in [1, 600]")
    if type(skew) is not int or not 0 <= skew <= 60:
        fail("oidc-clock-skew", "clock skew must be in [0, 60]")

    mappings = _exact_object(profile["claim_mappings"], set(WORKLOAD_FIELDS), "oidc-claim-mappings")
    for field, claim in mappings.items():
        if not isinstance(claim, str) or not CLAIM_RE.fullmatch(claim):
            fail("oidc-claim-name", f"invalid claim mapping for {field}")
    if len(set(mappings.values())) != len(mappings):
        fail("oidc-claim-alias", "one token claim cannot populate multiple workload fields")

    predicates = profile["predicates"]
    if not isinstance(predicates, dict) or not predicates:
        fail("oidc-predicates", "at least one immutable workload predicate is required")
    unknown = set(predicates) - set(WORKLOAD_FIELDS)
    if unknown:
        fail("oidc-predicates", f"unknown normalized fields: {sorted(unknown)}")
    required_predicates = {"repository_identity", "workflow_identity", "workflow_revision", "event_type"}
    if not required_predicates <= set(predicates):
        fail("oidc-predicates", f"missing immutable predicates: {sorted(required_predicates-set(predicates))}")
    if any(not isinstance(item, str) or not item for item in predicates.values()):
        fail("oidc-predicates", "predicate values must be non-empty strings")

    api = _exact_object(
        profile["repository_api"], {"base_url", "tls_spki_sha256", "response_schema"},
        "repository-api-shape",
    )
    if not _string(api["base_url"], "repository-api-url").startswith("https://"):
        fail("repository-api-url", "repository API must use HTTPS")
    if not isinstance(api["tls_spki_sha256"], list) or not api["tls_spki_sha256"]:
        fail("repository-api-pins", "repository API requires SPKI pins")
    if any(not isinstance(pin, str) or not HEX_64_RE.fullmatch(pin) for pin in api["tls_spki_sha256"]):
        fail("repository-api-pins", "invalid repository API SPKI pin")
    if api["response_schema"] != "aimee.candidate-target.v1":
        fail("repository-api-schema", "unsupported candidate-target schema")
    return profile


STANDARD_CLAIMS = {"iss", "sub", "aud", "iat", "nbf", "exp", "jti"}


def normalize_verified_oidc_claims(
    claims: object, profile: dict[str, object], *, wall_time: int
) -> dict[str, object]:
    """Normalize claims only after the caller verifies the JWT signature.

    The external verifier must reject redirects, select a configured algorithm,
    validate the pinned JWKS transport, and verify the JWS before calling this
    function.  Keeping that dependency injection explicit prevents this
    standard-library contract module from pretending to perform JOSE crypto.
    """
    if not isinstance(claims, dict):
        fail("oidc-claims", "verified claims must be an object")
    missing = STANDARD_CLAIMS - set(claims)
    if missing:
        fail("oidc-standard-claims", f"missing standard claims: {sorted(missing)}")
    if claims["iss"] != profile["issuer"]:
        fail("oidc-issuer-mismatch", "issuer does not match the selected profile")
    audience = claims["aud"]
    if audience != profile["audience"] and not (
        isinstance(audience, list) and audience == [profile["audience"]]
    ):
        fail("oidc-audience-mismatch", "audience must exactly match the profile")
    if not isinstance(claims["sub"], str) or not claims["sub"]:
        fail("oidc-subject", "subject must be a non-empty string")
    if not isinstance(claims["jti"], str) or not claims["jti"]:
        fail("oidc-token-id", "token ID must be a non-empty string")
    for field in ("iat", "nbf", "exp"):
        if type(claims[field]) is not int:
            fail("oidc-time-claim", f"{field} must be an integer NumericDate")
    iat = claims["iat"]
    nbf = claims["nbf"]
    exp = claims["exp"]
    if nbf > wall_time or wall_time > exp:
        fail("oidc-validity-window", "token is outside nbf/exp validity")
    if abs(wall_time - iat) > profile["clock_skew_seconds"]:
        fail("oidc-issued-at", "iat is outside the configured clock skew")
    if not 0 < exp - iat <= profile["max_token_lifetime_seconds"]:
        fail("oidc-token-lifetime", "token lifetime is invalid")

    workload: dict[str, object] = {
        "issuer": claims["iss"],
        "subject": claims["sub"],
        "audience": profile["audience"],
        "issued_at": iat,
        "not_before": nbf,
        "expires_at": exp,
        "token_id": claims["jti"],
    }
    mappings = profile["claim_mappings"]
    assert isinstance(mappings, dict)
    for field in WORKLOAD_FIELDS:
        claim_name = mappings[field]
        if claim_name not in claims:
            fail("oidc-mapped-claim", f"claim mapped to {field!r} is missing")
        value = claims[claim_name]
        if not isinstance(value, (str, int)) or isinstance(value, bool) or value == "":
            fail("oidc-mapped-claim", f"claim mapped to {field!r} has invalid type")
        workload[field] = str(value)
    predicates = profile["predicates"]
    assert isinstance(predicates, dict)
    for field, expected in predicates.items():
        if workload[field] != expected:
            fail("oidc-workload-predicate", f"normalized field {field!r} does not match policy")
    return workload


def validate_trigger_request(value: object) -> tuple[int, str]:
    request = _exact_object(value, {"schema", "pull_request", "oidc_token"}, "trigger-request-shape")
    if request["schema"] != "aimee.module-doc-trigger.v1":
        fail("trigger-request-schema", "unsupported trigger request schema")
    if type(request["pull_request"]) is not int or request["pull_request"] < 1:
        fail("trigger-request-pr", "pull_request must be a positive integer")
    token = _string(request["oidc_token"], "trigger-request-token")
    if len(token) > 16384:
        fail("trigger-request-token", "OIDC token exceeds 16384 bytes")
    return request["pull_request"], token


def validate_candidate_target(value: object) -> dict[str, str]:
    target = _exact_object(
        value,
        {"schema", "repository_identity", "pull_request", "protected_base_revision",
         "candidate_revision", "base_ref", "head_ref", "workflow_identity",
         "workflow_revision", "run_identity", "attempt", "trigger_check_identity"},
        "candidate-target-shape",
    )
    if target["schema"] != "aimee.candidate-target.v1":
        fail("candidate-target-schema", "unsupported candidate-target schema")
    if type(target["pull_request"]) is not int or target["pull_request"] < 1:
        fail("candidate-target-pr", "pull_request must be a positive integer")
    for field in ("protected_base_revision", "candidate_revision"):
        if not isinstance(target[field], str) or not re.fullmatch(r"[0-9a-f]{40}", target[field]):
            fail("candidate-target-revision", f"{field} must be a full lowercase commit ID")
    for field in (
        "repository_identity", "base_ref", "head_ref", "workflow_identity",
        "workflow_revision", "run_identity", "attempt", "trigger_check_identity",
    ):
        if not isinstance(target[field], str) or not target[field]:
            fail("candidate-target-field", f"{field} must be a non-empty string")
    return target


def bind_workload_to_target(
    workload: dict[str, object], target: dict[str, object], *, pull_request: int
) -> None:
    if target["pull_request"] != pull_request:
        fail("target-request-binding", "repository API returned a different pull request")
    bindings = {
        "repository_identity": "target-repository-binding",
        "workflow_identity": "target-workflow-binding",
        "workflow_revision": "target-workflow-binding",
        "run_identity": "target-run-binding",
        "attempt": "target-run-binding",
        "trigger_check_identity": "target-trigger-binding",
    }
    for field, rule in bindings.items():
        if target[field] != workload[field]:
            fail(rule, f"{field} mismatch")


def replay_binding(workload: dict[str, object], target: dict[str, object]) -> str:
    fields = (
        workload["issuer"], workload["token_id"], workload["repository_identity"],
        workload["run_identity"], workload["attempt"], workload["trigger_check_identity"],
        str(target["pull_request"]), target["protected_base_revision"],
        target["candidate_revision"],
    )
    if any(not isinstance(item, str) or not item for item in fields):
        fail("replay-binding", "binding fields must be non-empty strings")
    return sha256("\0".join(fields).encode("utf-8"))


def validate_v2_metadata(value: object, *, optional: bool) -> dict[str, object]:
    """Validate descriptor-v2 additions without enabling v2 production yet."""
    base = {"descriptor_version", "id", "dependencies", "runtime_toggle"}
    keys = base | {"docs", "sources", "public_headers", "surfaces"}
    if optional:
        keys.add("enabled_by_default")
    descriptor = _exact_object(value, keys, "v2-descriptor-shape")
    if descriptor["descriptor_version"] != 2 or type(descriptor["descriptor_version"]) is not int:
        fail("v2-descriptor-version", "descriptor_version must equal 2")
    module_id = descriptor["id"]
    if not isinstance(module_id, str) or not MODULE_ID_RE.fullmatch(module_id):
        fail("v2-module-id", "invalid module ID")
    if descriptor["docs"] != f"docs/modules/{module_id}.md":
        fail("v2-doc-path", "docs path must be derived from the module ID")
    for field in ("sources", "public_headers"):
        entries = descriptor[field]
        if not isinstance(entries, list) or entries != sorted(set(entries)):
            fail("v2-pattern-order", f"{field} must be a sorted unique array")
        if any(not isinstance(item, str) for item in entries):
            fail("v2-pattern-type", f"{field} entries must be strings")
    for pattern in descriptor["sources"]:
        match = SOURCE_PATTERN_RE.fullmatch(pattern)
        if not match or match.group("module") != module_id:
            fail("v2-source-pattern", f"invalid or cross-module source pattern {pattern!r}")
    if descriptor["public_headers"]:
        fail("v2-public-headers-deferred", "public headers must remain empty until canonical layout exists")
    surfaces = _exact_object(
        descriptor["surfaces"], {"routes", "commands", "protocols", "stages"},
        "v2-surfaces-shape",
    )
    for field, entries in surfaces.items():
        if entries != []:
            fail("v2-surfaces-deferred", f"{field} must remain empty until protected inventory exists")
    return descriptor


class DocumentProjection(NamedTuple):
    sources: tuple[str, ...]
    public_headers: tuple[str, ...]


class GitBlobReader:
    """Read governed files from one immutable commit, never a working tree."""

    def __init__(self, repository: Path, commit: str):
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            fail("git-candidate", "candidate must be a full lowercase commit ID")
        self.repository = repository
        self.commit = commit
        resolved = self._git("rev-parse", "--verify", f"{commit}^{{commit}}")
        if resolved.decode("ascii").strip() != commit:
            fail("git-candidate", "candidate does not resolve to itself")

    def _git(self, *arguments: str) -> bytes:
        result = subprocess.run(
            ["git", *arguments], cwd=self.repository, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
            env={"LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
        if result.returncode != 0:
            fail("git-object", result.stderr.decode("utf-8", "replace").strip())
        return result.stdout

    @staticmethod
    def canonical_path(path: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]*", path):
            fail("git-path", "path contains forbidden bytes")
        pure = PurePosixPath(path)
        if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
            fail("git-path", "path is not canonical")

    def read_blob(self, path: str) -> bytes:
        self.canonical_path(path)
        record = self._git("ls-tree", "-z", "--full-tree", self.commit, "--", path)
        entries = [entry for entry in record.split(b"\0") if entry]
        if len(entries) != 1:
            fail("git-entry", f"expected one entry for {path!r}, actual {len(entries)}")
        try:
            metadata, returned_path = entries[0].split(b"\t", 1)
            mode, kind, object_id = metadata.split(b" ", 2)
            decoded_path = returned_path.decode("ascii", "strict")
        except (ValueError, UnicodeDecodeError):
            fail("git-entry", f"malformed ls-tree record for {path!r}")
        if decoded_path != path:
            fail("git-entry", "repository returned a different path")
        if mode != b"100644" or kind != b"blob" or not re.fullmatch(rb"[0-9a-f]{40}", object_id):
            fail("git-mode", f"{path!r} must be a mode 100644 blob")
        raw = self._git("cat-file", "blob", object_id.decode("ascii"))
        return raw

    def resolve_reference(self, reference: str) -> None:
        match = REFERENCE_RE.fullmatch(reference)
        if not match:
            fail("document-evidence", f"invalid evidence reference {reference!r}")
        path = match.group("path")
        if not path:
            return
        raw = self.read_blob(path)
        if match.group("line") is not None:
            requested = int(match.group("line"))
            line_count = len(raw.splitlines())
            if requested > line_count:
                fail("document-evidence-line", f"line {requested} exceeds {line_count} lines in {path}")


def _validate_reference(
    reference: str, *, line: int, resolver: Callable[[str], None] | None = None
) -> None:
    match = REFERENCE_RE.fullmatch(reference)
    if not match:
        fail("document-evidence", f"invalid evidence reference {reference!r}", line=line)
    path = match.group("path")
    if path:
        pure = PurePosixPath(path)
        if pure.is_absolute() or ".." in pure.parts or "." in pure.parts or "//" in path:
            fail("document-evidence", "evidence path is not canonical", line=line)
    if resolver is not None:
        resolver(reference)


def parse_module_document(
    raw: bytes,
    module_id: str,
    projection: DocumentProjection,
    *,
    resolve_reference: Callable[[str], None] | None = None,
) -> None:
    """Parse the exact module-document language and enforce content floors."""
    if not MODULE_ID_RE.fullmatch(module_id):
        fail("document-module-id", "invalid module ID")
    if not raw or not raw.endswith(b"\n"):
        fail("document-final-lf", "document must end with one LF")
    if raw.endswith(b"\n\n"):
        fail("document-trailing-blank", "trailing blank line is forbidden")
    if any(byte not in (10,) and not 32 <= byte <= 126 for byte in raw):
        fail("document-byte-domain", "only printable ASCII and LF are allowed")
    lines = raw.decode("ascii").splitlines()
    if not lines or lines[0] != f"# {module_id} module":
        fail("document-h1", "H1 does not match module ID", line=1)
    if len(lines) < 2 or lines[1] != "":
        fail("document-record-spacing", "H1 must be followed by one blank line", line=2)

    positions: list[int] = []
    for title in SECTIONS:
        heading = f"## {title}"
        matches = [index for index, text in enumerate(lines) if text == heading]
        if len(matches) != 1:
            fail("document-section-cardinality", f"expected exactly one {heading!r}")
        positions.append(matches[0])
    if positions != sorted(positions):
        fail("document-section-order", "H2 sections are out of order")
    unexpected = [line for line in lines if line.startswith("#") and line not in {f"# {module_id} module", *(f"## {s}" for s in SECTIONS)}]
    if unexpected:
        fail("document-heading", f"unexpected heading {unexpected[0]!r}")

    total_tokens = 0
    for section_index, start in enumerate(positions):
        end = positions[section_index + 1] if section_index + 1 < len(positions) else len(lines)
        body = lines[start + 1:end]
        if not body or body[0] not in {"State: present", "State: none"}:
            fail("document-state", "state must immediately follow H2", line=start + 2)
        if section_index + 1 < len(positions) and (not body or body[-1] != ""):
            fail("document-record-spacing", "sections must be separated by one blank line", line=end)
        if body and body[-1] == "":
            body = body[:-1]
        if any(body[index] == "" and (index == 0 or index + 1 == len(body) or body[index - 1] == "" or body[index + 1] == "") for index in range(len(body))):
            fail("document-record-spacing", "blank lines may only separate records")
        records = [item for item in body if item]
        state = records.pop(0)
        if state == "State: none":
            if len(records) < 3 or not records[0].startswith("Reason: ") or not records[1].startswith("Implication: ") or not records[2].startswith("Evidence: "):
                fail("document-none-block", "none state requires Reason, Implication, Evidence in order", line=start + 3)
            for prefix in ("Reason: ", "Implication: "):
                prose = records[0 if prefix.startswith("Reason") else 1][len(prefix):]
                if not PROSE_RE.fullmatch(prose):
                    fail("document-prose", f"invalid {prefix.strip()} prose")
            _validate_reference(
                records[2][len("Evidence: "):], line=start + 5, resolver=resolve_reference
            )
            records = records[3:]
        elif any(item.startswith(("Reason: ", "Implication: ", "Evidence: ")) for item in records):
            fail("document-state-label", "none-state labels are forbidden for present state")

        expected_projection: list[str] = []
        if section_index == 0:
            expected_projection.append("Sources: " + (", ".join(projection.sources) or "none"))
        elif section_index == 2:
            expected_projection.append("Public headers: " + (", ".join(projection.public_headers) or "none"))
        elif section_index == 6:
            expected_projection.extend(("Routes: none", "Commands: none", "Protocols: none", "Stages: none"))
        if records[:len(expected_projection)] != expected_projection:
            fail("document-projection", f"projection mismatch in {SECTIONS[section_index]!r}")
        records = records[len(expected_projection):]

        prose_tokens = 0
        evidence_count = 0
        for record in records:
            if record.startswith("- Evidence: "):
                _validate_reference(
                    record[len("- Evidence: "):], line=start + 2, resolver=resolve_reference
                )
                evidence_count += 1
                continue
            prose = record[2:] if record.startswith("- ") else record
            if not PROSE_RE.fullmatch(prose):
                fail("document-prose", f"invalid record {record!r}")
            lowered = prose.casefold()
            words = {word.casefold() for word in TOKEN_RE.findall(prose)}
            if words & {"todo", "tbd", "placeholder"} or "coming soon" in lowered:
                fail("document-placeholder", "placeholder language is forbidden")
            prose_tokens += len(TOKEN_RE.findall(prose))
        if evidence_count < 1:
            fail("document-evidence-cardinality", "each section requires body evidence")
        if prose_tokens < 25:
            fail("document-section-word-floor", f"section has {prose_tokens} prose tokens; expected at least 25")
        total_tokens += prose_tokens
    if total_tokens < 600:
        fail("document-total-word-floor", f"document has {total_tokens} prose tokens; expected at least 600")


SUBJECT_KEYS = (
    "descriptor_sha256", "document_sha256", "module_id", "owner_identity",
    "reviewer_identity", "schema", "signed_at",
)


def parse_timestamp(value: object, rule: str) -> datetime:
    if not isinstance(value, str) or not TIMESTAMP_RE.fullmatch(value):
        fail(rule, "timestamp must be YYYY-MM-DDTHH:MM:SSZ")
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError as exc:
        fail(rule, f"invalid UTC Gregorian time: {exc}")
    return parsed


def canonical_subject(value: object) -> bytes:
    subject = _exact_object(value, set(SUBJECT_KEYS), "subject-shape")
    if subject["schema"] != "aimee.module-doc-attestation.v1":
        fail("subject-schema", "unsupported schema")
    if not isinstance(subject["module_id"], str) or not MODULE_ID_RE.fullmatch(subject["module_id"]):
        fail("subject-module", "invalid module ID")
    for field in ("descriptor_sha256", "document_sha256"):
        if not isinstance(subject[field], str) or not HEX_64_RE.fullmatch(subject[field]):
            fail("subject-hash", f"invalid {field}")
    for field in ("owner_identity", "reviewer_identity"):
        if not isinstance(subject[field], str) or not IDENTITY_RE.fullmatch(subject[field]):
            fail("subject-identity", f"invalid {field}")
    if subject["owner_identity"] == subject["reviewer_identity"]:
        fail("subject-separation", "owner and reviewer must be distinct")
    parse_timestamp(subject["signed_at"], "subject-signed-at")
    ordered = {key: subject[key] for key in SUBJECT_KEYS}
    return (json.dumps(ordered, indent=2, ensure_ascii=True, separators=(",", ": ")) + "\n").encode("ascii")


def validate_trust_policy(value: object, *, at: datetime) -> dict[str, dict[str, object]]:
    policy = _exact_object(value, {"schema", "epoch", "identities"}, "trust-policy-shape")
    if policy["schema"] != "aimee.module-doc-trust.v1":
        fail("trust-policy-schema", "unsupported schema")
    if type(policy["epoch"]) is not int or policy["epoch"] < 1:
        fail("trust-policy-epoch", "epoch must be a positive integer")
    identities = policy["identities"]
    if not isinstance(identities, list):
        fail("trust-policy-identities", "identities must be an array")
    result: dict[str, dict[str, object]] = {}
    keys: set[str] = set()
    for entry in identities:
        item = _exact_object(entry, {"identity", "role", "public_key", "not_before", "not_after", "revoked_at"}, "trust-policy-identity")
        identity = _string(item["identity"], "trust-policy-identity")
        if not IDENTITY_RE.fullmatch(identity) or identity in result:
            fail("trust-policy-identity", "identity is invalid or duplicated")
        if item["role"] not in {"owner", "reviewer"}:
            fail("trust-policy-role", "role must be owner or reviewer")
        public_key = _string(item["public_key"], "trust-policy-key")
        if not _valid_ed25519_public_key(public_key) or public_key in keys:
            fail("trust-policy-key", "key must be a unique canonical Ed25519 public key")
        keys.add(public_key)
        not_before = parse_timestamp(item["not_before"], "trust-policy-not-before")
        not_after = parse_timestamp(item["not_after"], "trust-policy-not-after")
        revoked = None if item["revoked_at"] is None else parse_timestamp(item["revoked_at"], "trust-policy-revoked-at")
        if not_before > at or at > not_after or (revoked is not None and at >= revoked):
            fail("trust-policy-authorization", f"identity {identity!r} is not authorized at verification time")
        result[identity] = item
    return result


TRUST_IDENTITY_KEYS = (
    "identity", "role", "public_key", "not_before", "not_after", "revoked_at"
)


def canonical_trust_policy(value: object, *, at: datetime) -> bytes:
    identities = validate_trust_policy(value, at=at)
    assert isinstance(value, dict)
    ordered_entries = [
        {key: identities[identity][key] for key in TRUST_IDENTITY_KEYS}
        for identity in sorted(identities)
    ]
    ordered = {
        "schema": value["schema"],
        "epoch": value["epoch"],
        "identities": ordered_entries,
    }
    return (json.dumps(ordered, indent=2, ensure_ascii=True, separators=(",", ": ")) + "\n").encode("ascii")


def load_canonical_trust_policy(raw: bytes, *, at: datetime) -> dict[str, dict[str, object]]:
    value = strict_json_bytes(raw)
    canonical = canonical_trust_policy(value, at=at)
    if raw != canonical:
        fail("trust-policy-canonical", "trust policy bytes are not canonical")
    return validate_trust_policy(value, at=at)


def validate_trust_epoch(previous: object, current: object) -> None:
    if type(previous) is not int or type(current) is not int or current <= previous:
        fail("trust-policy-epoch", "a trust-policy change must increase the protected epoch")


def _ssh_string(raw: bytes, offset: int) -> tuple[bytes, int]:
    if offset + 4 > len(raw):
        fail("sshsig-format", "truncated SSH string length")
    size = struct.unpack(">I", raw[offset:offset + 4])[0]
    start = offset + 4
    end = start + size
    if end > len(raw):
        fail("sshsig-format", "truncated SSH string")
    return raw[start:end], end


def _valid_ed25519_public_key(public_key: str) -> bool:
    parts = public_key.split(" ")
    if len(parts) != 2 or parts[0] != "ssh-ed25519" or not parts[1]:
        return False
    try:
        decoded = base64.b64decode(parts[1], validate=True)
        key_type, offset = _ssh_string(decoded, 0)
        key_bytes, offset = _ssh_string(decoded, offset)
    except (binascii.Error, ContractError):
        return False
    return key_type == b"ssh-ed25519" and len(key_bytes) == 32 and offset == len(decoded)


def _validate_sshsig_envelope(signature: bytes) -> None:
    begin = b"-----BEGIN SSH SIGNATURE-----\n"
    end = b"-----END SSH SIGNATURE-----\n"
    if not signature.startswith(begin) or not signature.endswith(end) or b"\r" in signature:
        fail("sshsig-armor", "signature must be canonical ASCII armor with final LF")
    encoded = signature[len(begin):-len(end)]
    lines = encoded.splitlines()
    if not lines or any(not line or len(line) > 76 for line in lines):
        fail("sshsig-armor", "signature armor has invalid line wrapping")
    try:
        decoded = base64.b64decode(b"".join(lines), validate=True)
    except binascii.Error:
        fail("sshsig-armor", "signature armor is not canonical base64")
    if not decoded.startswith(b"SSHSIG") or len(decoded) < 10:
        fail("sshsig-format", "missing SSHSIG magic")
    version = struct.unpack(">I", decoded[6:10])[0]
    if version != 1:
        fail("sshsig-format", "unsupported SSHSIG version")
    offset = 10
    public_blob, offset = _ssh_string(decoded, offset)
    namespace, offset = _ssh_string(decoded, offset)
    reserved, offset = _ssh_string(decoded, offset)
    hash_algorithm, offset = _ssh_string(decoded, offset)
    _, offset = _ssh_string(decoded, offset)
    if offset != len(decoded):
        fail("sshsig-format", "trailing SSHSIG bytes")
    key_type, key_offset = _ssh_string(public_blob, 0)
    key_bytes, key_offset = _ssh_string(public_blob, key_offset)
    if key_offset != len(public_blob) or key_type != b"ssh-ed25519" or len(key_bytes) != 32:
        fail("sshsig-key-type", "signature key must be Ed25519")
    if namespace != b"aimee.module-doc.v1" or reserved != b"":
        fail("sshsig-namespace", "signature namespace or reserved field is invalid")
    if hash_algorithm != b"sha512":
        fail("sshsig-hash", "signature hash algorithm must be SHA-512")


def authorize_subject(
    subject: dict[str, object], identities: dict[str, dict[str, object]], *, oidc_iat: int
) -> None:
    signed_at = parse_timestamp(subject["signed_at"], "subject-signed-at")
    verification_time = datetime.fromtimestamp(oidc_iat, tz=timezone.utc)
    age = (verification_time - signed_at).total_seconds()
    if age < 0 or age > 86400:
        fail("subject-freshness", "signed_at must be no later than iat and at most 24 hours old")
    expected = (("owner_identity", "owner"), ("reviewer_identity", "reviewer"))
    for field, role in expected:
        identity = subject[field]
        if identity not in identities:
            fail("subject-authorization", f"{field} is not enrolled")
        if identities[identity]["role"] != role:
            fail("subject-role", f"{field} is not authorized for role {role}")


def validate_toolchain_lock(value: object) -> dict[str, object]:
    lock = _exact_object(
        value, {"schema", "image", "ssh_keygen_sha256"}, "toolchain-lock-shape"
    )
    if lock["schema"] != "aimee.sshsig-toolchain.v1":
        fail("toolchain-lock-schema", "unsupported schema")
    image = _string(lock["image"], "toolchain-image")
    if not re.fullmatch(r"[a-z0-9./_-]+@sha256:[0-9a-f]{64}", image):
        fail("toolchain-image", "image must use an immutable sha256 digest")
    if not isinstance(lock["ssh_keygen_sha256"], str) or not HEX_64_RE.fullmatch(lock["ssh_keygen_sha256"]):
        fail("toolchain-binary", "ssh-keygen pin must be lowercase SHA-256")
    return lock


def verify_toolchain_binary(lock: dict[str, object], ssh_keygen: Path) -> None:
    try:
        actual = sha256(ssh_keygen.read_bytes())
    except OSError as exc:
        fail("toolchain-binary", f"cannot read ssh-keygen: {exc}")
    if actual != lock["ssh_keygen_sha256"]:
        fail("toolchain-binary", "ssh-keygen binary does not match deployment lock")


def validate_publisher_result(value: object, target: dict[str, object]) -> dict[str, object]:
    result = _exact_object(
        value,
        {"schema", "name", "candidate_revision", "external_id", "conclusion", "summary"},
        "publisher-result-shape",
    )
    if result["schema"] != "aimee.module-doc-check.v1" or result["name"] != "module-doc-attestation":
        fail("publisher-result-identity", "result must name the protected attestation check")
    if result["candidate_revision"] != target["candidate_revision"]:
        fail("publisher-result-candidate", "result targets a different candidate")
    if result["external_id"] != target["trigger_check_identity"]:
        fail("publisher-result-trigger", "external_id must bind the protected trigger check")
    if result["conclusion"] not in {"success", "failure"}:
        fail("publisher-result-conclusion", "conclusion must be success or failure")
    if not isinstance(result["summary"], str) or not 1 <= len(result["summary"]) <= 4096:
        fail("publisher-result-summary", "summary must contain 1 to 4096 characters")
    return result


def verify_sshsig(subject: bytes, signature: bytes, identity: str, public_key: str, ssh_keygen: Path) -> None:
    """Verify one Ed25519 SSHSIG without invoking a shell."""
    _validate_sshsig_envelope(signature)
    try:
        signature.decode("ascii", "strict")
    except UnicodeDecodeError:
        fail("sshsig-armor", "signature armor must be ASCII")
    if not IDENTITY_RE.fullmatch(identity) or not _valid_ed25519_public_key(public_key):
        fail("sshsig-signer", "signer identity or public key is invalid")
    if not ssh_keygen.is_absolute() or not ssh_keygen.is_file():
        fail("sshsig-tool", "ssh-keygen must be an absolute regular-file path")
    with tempfile.TemporaryDirectory(prefix="aimee-sshsig-") as tmp:
        root = Path(tmp)
        allowed = root / "allowed_signers"
        sig = root / "signature"
        allowed.write_text(f"{identity} {public_key}\n", encoding="ascii")
        sig.write_bytes(signature)
        result = subprocess.run(
            [str(ssh_keygen), "-Y", "verify", "-f", str(allowed), "-I", identity,
             "-n", "aimee.module-doc.v1", "-s", str(sig)],
            input=subject,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env={"LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
    if result.returncode != 0:
        fail("sshsig-verify", result.stderr.decode("utf-8", "replace").strip() or "signature rejected")


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()
