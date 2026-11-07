#!/usr/bin/env python3
"""Validate the core/process carving, placement, identities, and stage events."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = ROOT / "src/modules/process-contracts.json"
INVENTORY = ROOT / "tests/baselines/modules/canonical-inventory.yaml"
CORE = {
    "module-runtime", "ir", "translation", "protocols", "gateway",
    "vault", "audit", "observability",
}
PROCESS_REQUIRED = {
    "config", "memory", "learning", "routing", "delegates", "tools", "workspace", "git",
    "skills", "response-composition", "sandbox",
    # aimee is the core server module: everything specific to aimee-server,
    # including the nineteen store families it absorbed from db1 and the peer
    # messaging that was written against it while it was still its own module.
    # Required because a deployment without it is not a smaller deployment, and
    # a process because state belongs behind the bus.
    #
    # Being optional was also actively wrong -- an optional module with
    # enabled_by_default false is declared and never spawned, which is how peer
    # messaging came to be green in every test and absent from server.modules.
    "aimee", "egress", "providers",
    "execution-policy", "server", "kb", "postgres",
}
GO_PROCESSES = {
    "config", "memory", "learning", "routing", "delegates", "tools", "workspace", "git",
    "skills", "response-composition", "governance", "workflows", "roundtable", "kb-synthesis",
    "runtime-web", "control-web", "benchmarks", "sandbox", "economizer", "postgres",
    # The core server module. Its code is server-go/modules/aimee, and it is
    # Go-only: the store families were rewritten rather than migrated, and peer
    # messaging was created after the Go-first ruling in docs/dev/GO_REWRITE.md
    # so it never had a C implementation at all.
    #
    # It keeps principal ref 30 through the rename from db1: the ref is the
    # identity every grant matches on and the value the kind formula carves
    # from, so moving it would renumber all nineteen store event kinds and every
    # AIMEE_DB1_EVENT_* constant its 461 C call sites compile against. Peer
    # messaging renumbered instead -- it had no C callers to break, which is why
    # its stages are 20..23 rather than 1..4.
    "aimee", "egress", "providers",
    "execution-policy", "server", "kb",
}
# Executables that host a process other than the module runtime's multicall binary.
HOSTED_BY = {"wfe": "/usr/local/bin/aimee-wfe"}
# Exact internal service ownership, not permission to host arbitrary process
# stages in core. The C host uses a distinct trusted class0/ref0 serving slot.
HOST_STAGE_OWNERS = {("delegates", 23): (6679, "server-core")}

def process_stages(contract):
    return [stage for stage in contract["stages"] if "hosted_by" not in stage]


# A notification's subscriber is a process component, a client, or the C core,
# which subscribes its own host-owned slots and so needs no grant.
CORE_SUBSCRIBER = "core"


def notification_kinds(contract) -> list[int]:
    """Kinds a process component publishes (D14 change notifications)."""
    return [n["event_kind"] for n in contract.get("notifications", [])]


def subscriptions(document) -> dict[str, list[int]]:
    """Every subscriber's notification kinds, in contract order."""
    result: dict[str, list[int]] = {}
    for component in document.get("components", []):
        for notification in component.get("notifications", []):
            for subscriber in notification["subscribers"]:
                result.setdefault(subscriber, []).append(notification["event_kind"])
    return result


STAGE_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
PROTOCOL_KIND_FLAG = 0x80000000
MAX_MODULE_STAGES = 255


class ContractError(ValueError):
    pass


def load(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError(f"{path}: expected object")
    return value


def validate() -> dict[str, dict[str, object]]:
    inventory = load(INVENTORY)
    ordered = inventory.get("required", []) + inventory.get("optional", [])
    if not all(isinstance(item, str) for item in ordered):
        raise ContractError("canonical inventory is not a string array")
    contract = load(CONTRACTS)
    if set(contract) != {"schema_version", "principal_class", "components", "clients",
                         "telemetry"}:
        raise ContractError("top-level keys differ from v6")
    if contract["schema_version"] != 6 or contract["principal_class"] != 1:
        raise ContractError("schema_version must equal 6 and principal_class must equal 1")
    components = contract["components"]
    if not isinstance(components, list) or len(components) != len(ordered):
        raise ContractError("component count differs from canonical inventory")
    contract_ids = [item.get("id") for item in components if isinstance(item, dict)]
    if len(contract_ids) != len(set(contract_ids)):
        raise ContractError("components contains duplicate ids")
    # COVERAGE, not order. Order used to matter because the ref was the position;
    # now the ref is declared, so where a component sits in the file says nothing
    # and reordering it must not be a change to anyone's identity.
    if set(contract_ids) != set(ordered):
        raise ContractError("components must cover exactly the canonical inventory")

    result: dict[str, dict[str, object]] = {}
    refs: set[int] = set()
    kinds: set[int] = set()
    durability_kinds: set[int] = set()
    stage_names: set[str] = set()
    optional = set(inventory.get("optional", []))
    # The principal ref is DECLARED, not derived from position. It is a module's
    # permanent identity: grants match on it and event kinds are carved from it,
    # so it must survive a module changing classification or the list being
    # reordered. Deriving it from position welded "is this module essential" to
    # "which event kinds does it own", which meant promoting a module to required
    # renumbered every module after it.
    declared_refs = inventory.get("principal_refs")
    if not isinstance(declared_refs, dict):
        raise ContractError("canonical inventory must declare principal_refs")
    retired = inventory.get("retired_principal_refs")
    if not isinstance(retired, list):
        raise ContractError("canonical inventory must declare retired_principal_refs")
    for module_id, ref in declared_refs.items():
        if type(ref) is not int or ref < 1:
            raise ContractError(f"{module_id}: principal_ref must be a positive integer")
        if ref in retired:
            raise ContractError(
                f"{module_id}: principal_ref {ref} is retired and must never be reused")
    if len(set(declared_refs.values())) != len(declared_refs):
        raise ContractError("principal_refs contains duplicates")
    if set(declared_refs) != set(ordered):
        raise ContractError("principal_refs must cover exactly the canonical inventory")
    for ordinal, raw in enumerate(components, start=1):
        if not isinstance(raw, dict):
            raise ContractError(f"component {ordinal}: expected object")
        component_id = raw.get("id")
        execution = raw.get("execution")
        placements = raw.get("placements")
        if not isinstance(component_id, str) or execution not in {"core", "process"}:
            raise ContractError(f"component {ordinal}: invalid id/execution")
        placement_order = {"server": 0, "kb": 1}
        if (not isinstance(placements, list) or not placements or
                placements != sorted(set(placements), key=placement_order.get)):
            raise ContractError(f"{component_id}: placements must be sorted and unique")
        if not set(placements) <= {"kb", "server"}:
            raise ContractError(f"{component_id}: unknown placement")
        should_be_core = component_id in CORE
        if (execution == "core") != should_be_core:
            raise ContractError(f"{component_id}: execution violates the C core carving")
        if component_id in PROCESS_REQUIRED and execution != "process":
            raise ContractError(f"{component_id}: required feature must be a process")

        descriptor = load(ROOT / f"src/modules/{component_id}/module.yaml")
        # The C module-process build is retired: nothing reads c_build any more,
        # so a descriptor declaring one describes a binary that is never built.
        if "c_build" in descriptor or "c_init" in descriptor:
            raise ContractError(f"{component_id}: c_build/c_init are retired; module processes are Go")

        if execution == "core":
            if set(raw) != {"id", "execution", "placements"}:
                raise ContractError(f"{component_id}: core component has process fields")
        else:
            keys = set(raw) - {"hosted_by", "durability_declarations", "notifications"}
            if keys != {"id", "execution", "runtime", "principal_ref", "placements", "stages"}:
                raise ContractError(f"{component_id}: process keys differ from v5")
            # Most processes are multicall binaries the module runtime spawns and
            # pins by path. A process that is an already-supervised program instead
            # says so, because its grant must pin THAT executable and the runtime
            # must not spawn a second holder of the same principal -- a live
            # duplicate principal is denied admission.
            hosted_by = raw.get("hosted_by")
            if hosted_by is not None and hosted_by not in HOSTED_BY:
                raise ContractError(
                    f"{component_id}: hosted_by must be one of {sorted(HOSTED_BY)}")
            runtime = raw["runtime"]
            if runtime != "go":
                raise ContractError(f"{component_id}: process runtime must be go; every module "
                                    "process is Go and the C core is only their caller")
            if component_id not in GO_PROCESSES:
                raise ContractError(f"{component_id}: process is not in the Go process set")
            principal_ref = raw["principal_ref"]
            expected_ref = declared_refs[component_id]
            if type(principal_ref) is not int or principal_ref != expected_ref or principal_ref in refs:
                raise ContractError(
                    f"{component_id}: principal_ref must equal the inventory's declared "
                    f"{expected_ref}")
            refs.add(principal_ref)
            stages = raw["stages"]
            if not isinstance(stages, list) or not stages:
                raise ContractError(f"{component_id}: process must serve at least one stage")
            if len(stages) > MAX_MODULE_STAGES:
                raise ContractError(
                    f"{component_id}: process exceeds its {MAX_MODULE_STAGES}-stage carve")
            for stage_ordinal, stage in enumerate(stages, start=1):
                base_stage_keys = {
                    "id", "name", "event_kind", "durability", "durability_reason"
                }
                durability = stage.get("durability") if isinstance(stage, dict) else None
                expected_stage_keys = base_stage_keys | (
                    {"emitter", "sample_rate"} if durability == "sampled" else
                    {"emitter"} if durability == "ledger" else set())
                host_owner = HOST_STAGE_OWNERS.get((component_id, stage.get("id"))) if isinstance(stage, dict) else None
                if host_owner:
                    expected_stage_keys |= {"hosted_by"}
                    if stage.get("event_kind") != host_owner[0] or stage.get("hosted_by") != host_owner[1] or placements != ["server"]:
                        raise ContractError(f"{component_id}: invalid declared core stage owner")
                elif isinstance(stage, dict) and "hosted_by" in stage:
                    raise ContractError(f"{component_id}: stage has no declared core owner")
                if not isinstance(stage, dict) or set(stage) != expected_stage_keys:
                    raise ContractError(f"{component_id}: invalid stage shape")
                name, stage_id, kind = stage["name"], stage["id"], stage["event_kind"]
                reason = stage["durability_reason"]
                if durability not in {"ledger", "capture", "sampled"}:
                    raise ContractError(f"{component_id}/{name}: invalid durability class")
                if not isinstance(reason, str) or not reason.strip():
                    raise ContractError(f"{component_id}/{name}: durability reason is required")
                if durability in {"ledger", "sampled"} and \
                        stage["emitter"] != "obs_bus_emit_durable_event":
                    raise ContractError(f"{component_id}/{name}: invalid durability emitter")
                if durability == "sampled":
                    rate = stage["sample_rate"]
                    if (not isinstance(rate, (int, float)) or isinstance(rate, bool) or
                            not 0 < rate <= 1):
                        raise ContractError(f"{component_id}/{name}: invalid sample_rate")
                    if round(float(rate) * 1_000_000) != float(rate) * 1_000_000:
                        raise ContractError(
                            f"{component_id}/{name}: sample_rate must use ppm precision")
                # Carved from the DECLARED ref, so a module's event kinds move only
                # when its identity does -- which is never.
                discovery = stage_id == 255 and name == "public-command-discovery"
                expected_stage = 255 if discovery else stage_ordinal
                expected_kind = 4096 + principal_ref * 256 + expected_stage
                if expected_kind >= PROTOCOL_KIND_FLAG:
                    raise ContractError(
                        f"{component_id}/{name}: module stage enters protocol event namespace")
                if (type(stage_id) is not int or stage_id != expected_stage or
                        (discovery and (stage_ordinal != len(stages) or durability != "capture"))):
                    raise ContractError(f"{component_id}: stage IDs must be dense from one")
                if not isinstance(name, str) or not STAGE_RE.fullmatch(name) or name in stage_names:
                    raise ContractError(f"{component_id}: invalid or duplicate stage name {name!r}")
                if type(kind) is not int or kind != expected_kind or kind in durability_kinds:
                    raise ContractError(f"{component_id}/{name}: event_kind must equal {expected_kind}")
                stage_names.add(name)
                kinds.add(kind)
                durability_kinds.add(kind)
            declarations = raw.get("durability_declarations", [])
            if not isinstance(declarations, list):
                raise ContractError(
                    f"{component_id}: durability_declarations must be an array")
            reserved_ids: set[int] = set()
            for declaration in declarations:
                base_stage_keys = {
                    "id", "name", "event_kind", "durability", "durability_reason"
                }
                durability = declaration.get("durability") \
                    if isinstance(declaration, dict) else None
                expected_stage_keys = base_stage_keys | (
                    {"emitter", "sample_rate"} if durability == "sampled" else
                    {"emitter"} if durability == "ledger" else set())
                if not isinstance(declaration, dict) or \
                        set(declaration) != expected_stage_keys:
                    raise ContractError(
                        f"{component_id}: invalid durability declaration shape")
                name = declaration["name"]
                stage_id = declaration["id"]
                kind = declaration["event_kind"]
                reason = declaration["durability_reason"]
                if durability not in {"ledger", "capture", "sampled"}:
                    raise ContractError(f"{component_id}/{name}: invalid durability class")
                if not isinstance(reason, str) or not reason.strip():
                    raise ContractError(
                        f"{component_id}/{name}: durability reason is required")
                if durability in {"ledger", "sampled"} and \
                        declaration["emitter"] != "obs_bus_emit_durable_event":
                    raise ContractError(
                        f"{component_id}/{name}: invalid durability emitter")
                if durability == "sampled":
                    rate = declaration["sample_rate"]
                    if (not isinstance(rate, (int, float)) or isinstance(rate, bool) or
                            not 0 < rate <= 1):
                        raise ContractError(
                            f"{component_id}/{name}: invalid sample_rate")
                    if round(float(rate) * 1_000_000) != float(rate) * 1_000_000:
                        raise ContractError(
                            f"{component_id}/{name}: sample_rate must use ppm precision")
                if type(stage_id) is not int or \
                        not len(stages) < stage_id <= MAX_MODULE_STAGES:
                    raise ContractError(
                        f"{component_id}: reserved declaration IDs must follow active stages")
                expected_kind = 4096 + principal_ref * 256 + stage_id
                if expected_kind >= PROTOCOL_KIND_FLAG:
                    raise ContractError(
                        f"{component_id}/{name}: module stage enters protocol event namespace")
                if not isinstance(name, str) or not STAGE_RE.fullmatch(name) or \
                        name in stage_names:
                    raise ContractError(
                        f"{component_id}: invalid or duplicate stage name {name!r}")
                if type(kind) is not int or kind != expected_kind or kind in durability_kinds:
                    raise ContractError(
                        f"{component_id}/{name}: event_kind must equal {expected_kind}")
                stage_names.add(name)
                durability_kinds.add(kind)
                reserved_ids.add(stage_id)
            validate_notifications(component_id, raw, len(stages), reserved_ids, principal_ref,
                                   stage_names, durability_kinds)
            if component_id not in optional and component_id not in PROCESS_REQUIRED:
                raise ContractError(f"{component_id}: unexpected required process")
        result[component_id] = raw
    validate_clients(contract["clients"], kinds, refs)
    validate_telemetry(contract["telemetry"], contract["clients"], set(result), refs)
    validate_subscribers(contract)
    return result


def validate_notifications(component_id: str, raw: dict[str, object], stage_count: int,
                           reserved_ids: set[int], principal_ref: int, stage_names: set[str],
                           durability_kinds: set[int]) -> None:
    """Change notifications a process publishes (D14 in the ouroboros ring).

    A notification is carved from the publisher's own kind range, after its
    stages and durability declarations, so it can never collide with a stage.
    It carries ids and versions, never state, and the request that changed the
    state already wrote its durable row, so a notification is capture-only: it
    must never become a second WORM row per change.
    """
    notifications = raw.get("notifications", [])
    if not isinstance(notifications, list):
        raise ContractError(f"{component_id}: notifications must be an array")
    keys = {"id", "name", "event_kind", "durability", "durability_reason", "subscribers"}
    for notification in notifications:
        if not isinstance(notification, dict) or set(notification) != keys:
            raise ContractError(f"{component_id}: invalid notification shape")
        name = notification["name"]
        notification_id = notification["id"]
        kind = notification["event_kind"]
        if type(notification_id) is not int or \
                not stage_count < notification_id <= MAX_MODULE_STAGES or \
                notification_id in reserved_ids:
            raise ContractError(
                f"{component_id}: notification IDs must follow active stages and declarations")
        reserved_ids.add(notification_id)
        if not isinstance(name, str) or not STAGE_RE.fullmatch(name) or name in stage_names:
            raise ContractError(f"{component_id}: invalid or duplicate notification name {name!r}")
        expected_kind = 4096 + principal_ref * 256 + notification_id
        if type(kind) is not int or kind != expected_kind or kind in durability_kinds:
            raise ContractError(f"{component_id}/{name}: event_kind must equal {expected_kind}")
        if notification["durability"] != "capture":
            raise ContractError(
                f"{component_id}/{name}: a notification is capture-only; the change it "
                "announces is already durable")
        reason = notification["durability_reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise ContractError(f"{component_id}/{name}: durability reason is required")
        subscribers = notification["subscribers"]
        if (not isinstance(subscribers, list) or not subscribers or
                any(not isinstance(item, str) or not item for item in subscribers) or
                len(subscribers) != len(set(subscribers)) or component_id in subscribers):
            raise ContractError(
                f"{component_id}/{name}: subscribers must be unique names, not the publisher")
        stage_names.add(name)
        durability_kinds.add(kind)


def validate_subscribers(contract: dict[str, object]) -> None:
    """A subscriber that names nothing is a grant that admits nothing."""
    components = contract["components"]
    clients = contract["clients"]
    assert isinstance(components, list) and isinstance(clients, list)
    known = {CORE_SUBSCRIBER}
    known |= {item["id"] for item in components if item.get("execution") == "process"}
    known |= {item["id"] for item in clients}
    for subscriber in subscriptions(contract):
        if subscriber not in known:
            raise ContractError(f"notification subscriber {subscriber!r} is not a process, "
                                "a client or the core")


def validate_telemetry(telemetry: object, clients: list, component_ids: set[str],
                       module_refs: set[int]) -> None:
    """The ring's telemetry exporter (ouroboros ring D16).

    The host hands one principal the consumer end of its telemetry ring, which
    describes every event on the ring, so exactly one is declared. It serves and
    requests nothing; its grant says telemetry=yes, and the supervisor runs it
    in every placement that hosts a ring.
    """
    if not isinstance(telemetry, dict) or set(telemetry) != {
            "id", "principal_ref", "executable", "placements"}:
        raise ContractError("telemetry: keys differ from v6")
    identifier = telemetry["id"]
    client_ids = {client["id"] for client in clients}
    if (not isinstance(identifier, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", identifier)
            or identifier in client_ids or identifier in component_ids):
        raise ContractError("telemetry: invalid or colliding id")
    ref = telemetry["principal_ref"]
    if type(ref) is not int or ref <= max(module_refs, default=0):
        raise ContractError("telemetry: principal_ref must sit above every module ref")
    if ref in module_refs or ref in {client["principal_ref"] for client in clients}:
        raise ContractError(f"telemetry: principal_ref {ref} collides")
    executable = telemetry["executable"]
    if executable != f"/usr/local/libexec/aimee-modules/aimee-module-{identifier}":
        raise ContractError("telemetry: executable must be the module runtime under its id")
    placements = telemetry["placements"]
    if placements != ["server", "kb"]:
        raise ContractError("telemetry: every placement hosts a ring, so both run the exporter")


def validate_clients(clients: object, served_kinds: set[int], module_refs: set[int]) -> None:
    """Bus principals that request stages and may publish declared audit actions.

    A client is not a module: it serves nothing and has no inventory ordinal, so
    its principal_ref cannot be derived from one. Refs are therefore explicit and
    live above the module range, and a client may only request a kind some module
    actually serves -- a typo there is a grant that admits nothing, which shows
    up as an unexplained runtime refusal rather than a build failure.
    """
    if not isinstance(clients, list):
        raise ContractError("clients must be an array")
    seen_ids: set[str] = set()
    seen_refs: set[int] = set()
    for ordinal, client in enumerate(clients, start=1):
        if not isinstance(client, dict) or set(client) - {"publish"} != {
                "id", "principal_ref", "executable", "placements", "request"}:
            raise ContractError(f"client {ordinal}: keys differ from v3")
        identifier = client["id"]
        if not isinstance(identifier, str) or not identifier or identifier in seen_ids:
            raise ContractError(f"client {ordinal}: invalid or duplicate id")
        seen_ids.add(identifier)
        ref = client["principal_ref"]
        if type(ref) is not int or ref <= max(module_refs, default=0):
            raise ContractError(f"{identifier}: principal_ref must sit above every module ref")
        if ref in seen_refs or ref in module_refs:
            raise ContractError(f"{identifier}: principal_ref {ref} collides")
        seen_refs.add(ref)
        if ref == 76 or identifier == "delegates-host":
            if client != {"id":"delegates-host","principal_ref":76,"executable":"/usr/local/libexec/aimee-modules/aimee-module-delegates","placements":["server"],"request":[6679]}:
                raise ContractError("delegates-host must be request-only principal76 for native host kind6679")
        executable = client["executable"]
        if not isinstance(executable, str) or not executable.startswith("/"):
            raise ContractError(f"{identifier}: executable must be an absolute path")
        placements = client["placements"]
        placement_order = {"server": 0, "kb": 1}
        if (not isinstance(placements, list) or not placements or
                placements != sorted(set(placements), key=placement_order.get)):
            raise ContractError(f"{identifier}: placements must be sorted and unique")
        if not set(placements) <= {"kb", "server"}:
            raise ContractError(f"{identifier}: unknown placement")
        request = client["request"]
        if not isinstance(request, list) or not request:
            raise ContractError(f"{identifier}: a client must request at least one kind")
        for kind in request:
            if type(kind) is not int or kind not in served_kinds:
                raise ContractError(f"{identifier}: requests kind {kind}, which no module serves")
        # ACTION is a daemon-owned notification, not an AMOD request stage.
        # Keep publication separate from request grants and limited to the
        # existing content-free observability wire (obs_bus.h).
        publish = client.get("publish", [])
        if (not isinstance(publish, list) or
                any(type(kind) is not int or kind != 3000 for kind in publish) or
                len(publish) != len(set(publish))):
            raise ContractError(f"{identifier}: publish may only contain audit ACTION kind 3000")


def main() -> int:
    try:
        components = validate()
    except (OSError, UnicodeError, json.JSONDecodeError, ContractError) as exc:
        print(f"validate_module_process_contracts: error: {exc}", file=sys.stderr)
        return 1
    process_count = sum(item["execution"] == "process" for item in components.values())
    print(f"validate_module_process_contracts: ok ({len(components)} components, "
          f"{process_count} Go processes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
