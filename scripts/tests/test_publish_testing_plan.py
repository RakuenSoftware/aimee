#!/usr/bin/env python3
"""Regression tests for selective testing-image publication."""

from __future__ import annotations

import importlib.util
import pathlib
import re
import subprocess
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
        kb,
        "KB runtime C source",
    )
    failures += expect(["src/cli_argspec.c"], set(), "thin-client-only C source")
    failures += expect(
        [
            "api/openapi-server-v1.yaml",
            "src/server/server_state.c",
            "src/tests/test_integration.sh",
        ],
        set(planner.SERVER),
        "#2892 server and server OpenAPI change",
    )
    failures += expect(
        ["src/server/oauth_pkce.c"],
        set(planner.SERVER | planner.KB),
        "server leaf linked into KB",
    )
    failures += expect(
        ["src/modules/memory/memory_data_bus.c"],
        set(planner.SERVER | planner.KB),
        "shared server and KB C source",
    )
    failures += expect(
        ["src/kb/kb_mgmt_token_roots_provision_main.c"],
        set(planner.AUTHORITY),
        "authority-only C source",
    )
    failures += expect(
        ["src/headers/server_skill.h"],
        set(planner.SERVER),
        "server-only header include closure",
    )
    failures += expect(
        ["api/openapi-v1.yaml", "src/gen_openapi.py"],
        kb,
        "KB OpenAPI inputs",
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
        [
            ".github/workflows/ci.yml",
            ".github/workflows/publish-testing.yml",
            "scripts/check-published-compose-images.py",
            "scripts/publish_testing_plan.py",
            "scripts/tests/test_check_published_compose_images.py",
            "scripts/tests/test_publish_testing_plan.py",
        ],
        set(),
        "this publication-only PR does not rebuild runtime images",
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

    # Independently expand the actual shipping KB targets and ensure every direct
    # server leaf is classified as a KB consumer by the transitive graph.
    make_db = subprocess.run(
        ["make", "-C", str(ROOT / "src"), "-pnRrq"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    ).stdout
    kb_target_lines = "\n".join(
        line
        for line in make_db.splitlines()
        if line.startswith("../aimee-kb:") or line.startswith("../aimee-kb-worm:")
    )
    linked_server_sources = {
        f"src/server/{name}.c"
        for name in re.findall(r"build/obj/server/([A-Za-z0-9_./-]+)\.o", kb_target_lines)
    }
    misclassified = sorted(
        path for path in linked_server_sources if not planner.KB.issubset(planner.consumers(path))
    )
    if misclassified:
        print(
            "  FAIL  KB-linked server sources are not classified for KB: "
            f"{misclassified}"
        )
        failures += 1
    else:
        print("  ok    KB-linked server sources are classified from the Make graph")

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
