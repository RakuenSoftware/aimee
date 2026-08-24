#!/usr/bin/env python3
"""Fail closed unless every registered bus kind has a durability contract."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = Path("src/modules/process-contracts.json")
OBS_BUS = Path("src/modules/audit/obs_bus.c")
CLASSES = {"ledger", "capture", "sampled"}
DEFINE_RE = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*)\s+([A-Z][A-Z0-9_]*|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*)\s*$", re.M)
EVENT_RE = re.compile(r"^AIMEE_[A-Z0-9_]+_EVENT_[A-Z0-9_]+$")
LEDGER_BLOCK_RE = re.compile(r"LEDGER_EVENT_KINDS\[\]\s*=\s*\{(.*?)\n\};", re.S)
SAMPLED_BLOCK_RE = re.compile(r"SAMPLED_EVENT_KINDS\[\]\s*=\s*\{(.*?)\};", re.S)


class DurabilityError(ValueError):
    pass


def _literal(value: str) -> int | None:
    trimmed = re.sub(r"[uUlL]+$", "", value)
    try:
        return int(trimmed, 0)
    except ValueError:
        return None


def resolve_header_kinds(text: str) -> dict[str, int]:
    """Resolve numeric and same-header alias EVENT_* defines."""
    definitions = dict(DEFINE_RE.findall(text))
    resolved: dict[str, int] = {}

    def resolve(name: str, visiting: set[str]) -> int | None:
        if name in resolved:
            return resolved[name]
        if name in visiting or name not in definitions:
            return None
        token = definitions[name]
        value = _literal(token)
        if value is None:
            value = resolve(token, visiting | {name})
        if value is not None:
            resolved[name] = value
        return value

    for name in definitions:
        if EVENT_RE.fullmatch(name):
            resolve(name, set())
    return {name: resolved[name] for name in definitions if EVENT_RE.fullmatch(name) and name in resolved}


def analyse(root: Path = ROOT) -> tuple[dict[str, int], list[str]]:
    errors: list[str] = []
    try:
        contract = json.loads((root / CONTRACTS).read_text(encoding="utf-8"))
        obs_text = (root / OBS_BUS).read_text(encoding="utf-8")
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return {}, [str(exc)]

    stages: list[tuple[str, dict[str, object]]] = []
    registered_processes: set[str] = set()
    for component in contract.get("components", []):
        if not isinstance(component, dict):
            errors.append("process contract contains a non-object component")
            continue
        if component.get("execution") == "process":
            module_id = component.get("id")
            if isinstance(module_id, str):
                registered_processes.add(module_id)
                active = component.get("stages", [])
                reserved = component.get("durability_declarations", [])
                if not isinstance(active, list) or not isinstance(reserved, list):
                    errors.append(f"{module_id}: durability declarations are not arrays")
                    continue
                declarations = active + reserved
                for stage in declarations:
                    if isinstance(stage, dict):
                        stages.append((module_id, stage))

    declared: dict[int, tuple[str, dict[str, object]]] = {}
    for module_id, stage in stages:
        kind = stage.get("event_kind")
        label = f"{module_id}/{stage.get('name', '?')}"
        if type(kind) is not int:
            errors.append(f"{label}: event_kind is not an integer")
            continue
        if kind in declared:
            errors.append(f"{label}: duplicate event_kind {kind}")
        else:
            declared[kind] = (label, stage)
        durability = stage.get("durability")
        reason = stage.get("durability_reason")
        if durability not in CLASSES:
            errors.append(f"{label}: missing or invalid durability")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"{label}: missing durability_reason")
        if durability in {"ledger", "sampled"} and stage.get("emitter") != "obs_bus_emit_durable_event":
            errors.append(f"{label}: {durability} kind has no checked emitter")
        if durability == "sampled":
            rate = stage.get("sample_rate")
            if not isinstance(rate, (int, float)) or isinstance(rate, bool) or not 0 < rate <= 1:
                errors.append(f"{label}: sampled kind needs sample_rate in (0, 1]")
            elif round(float(rate) * 1_000_000) != float(rate) * 1_000_000:
                errors.append(f"{label}: sample_rate must use parts-per-million precision")

    header_symbols = 0
    header_kinds: set[int] = set()
    for module_id in sorted(registered_processes):
        header = root / "src/modules" / module_id / "include" / "aimee" / module_id / "module_api.h"
        if not header.exists():
            continue  # C db1 exposes its generated family contract elsewhere.
        resolved = resolve_header_kinds(header.read_text(encoding="utf-8"))
        header_symbols += len(resolved)
        for symbol, kind in resolved.items():
            header_kinds.add(kind)
            if kind not in declared:
                errors.append(f"{header.relative_to(root)}: {symbol} resolves to undeclared kind {kind}")

    if header_symbols == 0 or not header_kinds:
        errors.append("zero module_api event kinds resolved (refusing vacuous success)")

    ledger_declared = {kind for kind, (_, stage) in declared.items() if stage.get("durability") == "ledger"}
    sampled_declared = {kind for kind, (_, stage) in declared.items() if stage.get("durability") == "sampled"}
    match = LEDGER_BLOCK_RE.search(obs_text)
    runtime_ledger = set(map(int, re.findall(r"\{\s*(\d+)u?\s*,", match.group(1)))) if match else set()
    if not match:
        errors.append("obs_bus.c has no LEDGER_EVENT_KINDS runtime table")
    for kind in sorted(ledger_declared - runtime_ledger):
        errors.append(f"ledger kind {kind} is absent from the runtime emitter table")
    for kind in sorted(runtime_ledger - ledger_declared):
        errors.append(f"runtime emitter kind {kind} is not declared ledger")
    sampled_match = SAMPLED_BLOCK_RE.search(obs_text)
    runtime_sampled = {
        int(kind): int(rate)
        for kind, rate in re.findall(
            r'\{\s*(\d+)u?\s*,\s*"[^"]+"\s*,\s*(\d+)u?\s*\}',
            sampled_match.group(1) if sampled_match else "")
    }
    sampled_rates = {
        kind: round(float(stage.get("sample_rate", 0)) * 1_000_000)
        for kind, (_, stage) in declared.items()
        if stage.get("durability") == "sampled"
    }
    for kind in sorted(sampled_declared - runtime_sampled.keys()):
        errors.append(f"sampled kind {kind} is absent from the runtime sampler table")
    for kind in sorted(runtime_sampled.keys() - sampled_declared):
        errors.append(f"runtime sampler kind {kind} is not declared sampled")
    for kind in sorted(sampled_declared & runtime_sampled.keys()):
        if runtime_sampled[kind] != sampled_rates[kind]:
            errors.append(
                f"sampled kind {kind} runtime rate {runtime_sampled[kind]} ppm "
                f"differs from declared {sampled_rates[kind]} ppm")
    if "obs_bus_emit_durable_event(\"bus.module.request\"" not in obs_text or \
            "obs_bus_emit_durable_event(\"bus.module.reply\"" not in obs_text:
        errors.append("module request/reply durable emitter path is absent")

    counts = {
        "header_symbols": header_symbols,
        "header_kinds": len(header_kinds),
        "declared": len(declared),
        "ledger": len(ledger_declared),
        "capture": sum(stage.get("durability") == "capture" for _, stage in stages),
        "sampled": len(sampled_declared),
    }
    return counts, errors


def main() -> int:
    counts, errors = analyse()
    if errors:
        for error in errors:
            print(f"check_event_durability: error: {error}", file=sys.stderr)
        return 1
    print("check_event_durability: ok "
          f"({counts['header_kinds']} header kinds from {counts['header_symbols']} symbols, "
          f"{counts['declared']} declared: {counts['ledger']} ledger, "
          f"{counts['capture']} capture, {counts['sampled']} sampled)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
