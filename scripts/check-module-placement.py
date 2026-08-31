#!/usr/bin/env python3
"""Every module stage a daemon CALLS must be served by a module PLACED in it.

Written for a gap that had shipped: aimee-kb called the memory module's
extraction, write-gate and embed stages, and the manifest placed that module in
`server` only. Nothing failed loudly -- obs_bus_module_call returns TRANSPORT,
the adapters turn that into "no answer", and the whole typed-fact layer silently
did nothing. Fail-closed, and invisible: one WARN per turn, and no deployment
noticed.

This pairs the two sides that were never compared: the call sites in each
daemon's C sources, and the placements in the runtime-bundle manifest.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK = ROOT / "dependencies/aimee-repositories.lock.json"

# Which C sources belong to which daemon's module-client surface. Both files
# exist for the same reason -- each daemon registers its own provider table --
# and each may only reach modules placed in its own daemon.
CALLERS = {
    "kb": ROOT / "src/kb/kb_module_stage_adapters.c",
    "server": ROOT / "src/server/module_stage_adapters.c",
}

# The event-kind constants are spelled as macros at the call sites; resolve them
# from the headers that define them rather than hardcoding a second copy.
EVENT_DEF = re.compile(r"^#define\s+(AIMEE_[A-Z0-9_]*_EVENT_[A-Z0-9_]+)\s+(\d+)u?\s*(?:/\*.*)?$",
                       re.MULTILINE)
CALL_SITE = re.compile(r"call_module(?:_with_budget)?\s*\(\s*(AIMEE_[A-Z0-9_]+)")

# No known gaps. The list below is deliberately empty: it exists so a placement
# gap that cannot be fixed immediately can be recorded rather than ignored, and
# the stale-entry check further down means an entry cannot outlive its fix.
#
# It was populated once, by the gap this check was written for: aimee-kb calls
# the memory module's extraction, write-gate and embed stages, and the manifest
# placed that module in `server` only, so the typed-fact layer silently did
# nothing in a deployed kb. That is fixed -- `memory` is now placed in both.
KNOWN_GAPS: set[tuple[str, str]] = set()


def event_constants() -> dict[str, int]:
    found: dict[str, int] = {}
    for header in ROOT.rglob("src/modules/**/include/**/*.h"):
        for name, value in EVENT_DEF.findall(header.read_text(encoding="utf-8", errors="replace")):
            found[name] = int(value)
    return found


def main() -> int:
    if not LOCK.exists():
        print(f"check-module-placement: missing {LOCK}", file=sys.stderr)
        return 2
    manifest = json.loads(LOCK.read_text())
    constants = event_constants()

    served_by_placement: dict[str, set[int]] = {}
    owner: dict[int, str] = {}
    for module in manifest["modules"]:
        for event in module.get("serve") or []:
            owner[event] = module["id"]
            for placement in module.get("placements") or []:
                served_by_placement.setdefault(placement, set()).add(event)

    failures = []
    seen_known = set()
    for daemon, source in CALLERS.items():
        if not source.exists():
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for macro in sorted(set(CALL_SITE.findall(text))):
            event = constants.get(macro)
            if event is None:
                print(f"check-module-placement: {daemon}: unresolved event macro {macro}",
                      file=sys.stderr)
                return 2
            if event in served_by_placement.get(daemon, set()):
                continue
            if (daemon, macro) in KNOWN_GAPS:
                seen_known.add((daemon, macro))
                continue
            who = owner.get(event, "<no module serves it>")
            failures.append(
                f"  {daemon} calls {macro} ({event}) but module '{who}' is not placed in "
                f"'{daemon}' (placements: "
                f"{[m.get('placements') for m in manifest['modules'] if m['id'] == who]})")

    if failures:
        print("check-module-placement: a daemon calls a stage no module placed in it serves.",
              file=sys.stderr)
        print("The call fails as TRANSPORT and the feature silently does nothing.", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    # A gap that has been fixed must leave the list, or the list stops meaning
    # anything: a stale entry would forgive a regression that reintroduces it.
    stale = KNOWN_GAPS - seen_known
    if stale:
        print("check-module-placement: KNOWN_GAPS lists placements that are no longer missing; "
              "remove them:", file=sys.stderr)
        for daemon, macro in sorted(stale):
            print(f"  {daemon} {macro}", file=sys.stderr)
        return 1

    print(f"check-module-placement: ok ({len(CALLERS)} daemons, "
          f"{sum(len(v) for v in served_by_placement.values())} placed stages, "
          f"{len(seen_known)} known gap(s) recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
