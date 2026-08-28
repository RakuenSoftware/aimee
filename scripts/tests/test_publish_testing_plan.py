#!/usr/bin/env python3
"""Regression tests for selective testing-image publication."""

from __future__ import annotations

import importlib.util
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "publish_testing_plan.py"
WORKFLOW = ROOT / ".github" / "workflows" / "publish-testing.yml"
MAKEFILE = ROOT / "src" / "Makefile"

spec = importlib.util.spec_from_file_location("publish_testing_plan", SCRIPT)
if spec is None or spec.loader is None:
    sys.exit("cannot load publish_testing_plan.py")
planner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = planner
spec.loader.exec_module(planner)


def rebuilt(paths: list[str]) -> set[str]:
    return {item["name"] for item in planner.plan(paths) if item["rebuild"]}


def expect(paths: list[str], want: set[str], why: str) -> int:
    got = rebuilt(paths)
    ok = got == want
    print(("  ok    " if ok else "  FAIL  ") + f"{why}: {sorted(got)}")
    if not ok:
        print(f"        expected: {sorted(want)}")
    return 0 if ok else 1


def dockerfile_container_inputs(path: pathlib.Path) -> set[str]:
    """Literal deploy/container inputs copied by a Dockerfile."""

    found: set[str] = set()
    instructions: list[str] = []
    current = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        current = f"{current} {line.strip()}".strip()
        if current.endswith("\\"):
            current = current[:-1]
            continue
        instructions.append(current)
        current = ""
    if current:
        instructions.append(current)
    for instruction in instructions:
        if not instruction.startswith("COPY "):
            continue
        found.update(re.findall(r"deploy/container/[A-Za-z0-9_.-]+", instruction))
    return found


def main() -> None:
    failures = 0
    all_images = set(planner.ALL)
    kb = set(planner.KB)

    # The reported incident: a server-side KB client diagnostic and a test must not
    # rebuild any KB image, especially the multi-gigabyte A25M/Nomic variants.
    failures += expect(
        ["src/modules/kb_client/kb_client_mtls.c", "src/tests/test_audit_worm.c"],
        set(planner.SERVER),
        "#2887 server-only change",
    )
    failures += expect(
        ["src/kb/kb_service.c"],
        set(planner.SERVER | planner.KB | planner.AUTHORITY),
        "shared C source is conservative",
    )
    failures += expect(
        ["frontend/src/App.tsx"],
        set(planner.SERVER | planner.CONTROL),
        "shared browser frontend",
    )
    failures += expect(
        ["control-web/main.go"], set(planner.CONTROL), "control console only"
    )
    failures += expect(
        ["deploy/container/aimee-kb-entrypoint.sh"], kb, "KB entrypoint only"
    )
    failures += expect(
        ["deploy/container/module-supervisor.sh"],
        set(planner.SERVER | planner.KB),
        "shared container supervisor",
    )
    failures += expect(
        ["src/tests/test_kb_http.c", "scripts/tests/test_bake_embedder.py"],
        set(),
        "tests are not runtime inputs",
    )
    failures += expect(
        [".github/workflows/publish-testing.yml", "scripts/publish_testing_plan.py"],
        set(),
        "publication orchestration is not image content",
    )
    failures += expect(["new/runtime/input"], all_images, "unknown input fails safe")

    forced = {item["name"] for item in planner.plan([], force_all=True) if item["rebuild"]}
    if forced != all_images:
        print(f"  FAIL  manual recovery: {sorted(forced)}")
        failures += 1
    else:
        print("  ok    manual recovery rebuilds all images")

    # Enforce the ownership fact that makes the narrow kb_client rule safe.
    makefile = MAKEFILE.read_text(encoding="utf-8")
    # The aggregate may appear elsewhere under its server-owned alias, but the raw
    # object group must have exactly one consumer: SERVER_KB_CLIENT_OBJS.  A KB target
    # taking it directly makes this fail until the image contract is widened.
    raw_consumers = re.findall(r"^.*\$\(KB_CLIENT_OBJS\).*$", makefile, re.M)
    want_consumer = ["SERVER_KB_CLIENT_OBJS = $(KB_CLIENT_OBJS)"]
    if raw_consumers != want_consumer:
        print(f"  FAIL  KB_CLIENT_OBJS gained a new consumer: {raw_consumers}")
        failures += 1
    else:
        print("  ok    KB_CLIENT_OBJS remains server-owned")

    # If a Dockerfile starts copying another container file, the contract must be
    # updated in the same change.  This prevents selective publishing from drifting.
    docker_contracts = (
        ("Dockerfile", planner.KB_CONTAINER_FILES),
        ("Dockerfile.server", planner.SERVER_CONTAINER_FILES),
        ("Dockerfile.authority-bootstrap", planner.AUTHORITY_CONTAINER_FILES),
    )
    for filename, declared in docker_contracts:
        actual = dockerfile_container_inputs(ROOT / filename)
        missing = actual - set(declared)
        if missing:
            print(f"  FAIL  {filename} has undeclared container inputs: {sorted(missing)}")
            failures += 1
        else:
            print(f"  ok    {filename} container inputs are declared")

    workflow = WORKFLOW.read_text(encoding="utf-8")
    required_workflow_fragments = (
        "scripts/publish_testing_plan.py --null",
        "docker manifest inspect",
        "docker buildx imagetools create",
        "steps.have.outputs.exists != 'true'",
        "steps.have.outputs.exists == 'true'",
    )
    for fragment in required_workflow_fragments:
        if fragment not in workflow:
            print(f"  FAIL  workflow does not enforce {fragment!r}")
            failures += 1
        else:
            print(f"  ok    workflow contains {fragment!r}")

    if failures:
        sys.exit(f"test_publish_testing_plan: {failures} failure(s)")
    print("test_publish_testing_plan: ok")


if __name__ == "__main__":
    main()
