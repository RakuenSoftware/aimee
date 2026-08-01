#!/usr/bin/env python3
"""Every variable the deploy layer EMITS must be CONSUMED by something.

config_emit_deploy_env() writes the .env that the Compose stack is started with. A
key it emits that nothing reads is not a harmless leftover — it is a setting the
user configured that silently does nothing. This has now happened three times while
retiring the aimee-llm container:

  * EMBEDDER_MODEL      emitted, but absent from every aimee-kb service environment,
                        so the wizard's embedder selection never reached the
                        container. The entrypoint saw no selection, started nothing,
                        and the builtin lexical embedder served forever. The headline
                        feature of the cutover, dead end to end.
  * EMBEDDER_URL  same: an external embedder could be configured and would
                        never be used.
  * AIMEE_LLM_SYNTH_URL emitted for an external synth endpoint, read by nobody — it
                        had been the retired gateway's own knob.

None of these fail a build, a unit test, or a YAML parse. Two of them were found
only because a live topology smoke asserted the kb could actually embed; the third
was found by reading. So the invariant is asserted statically here instead.

A key counts as consumed if it is read in C (getenv / runtime_secret_get), read in
a shell script, or passed to a service in a shipped Compose file. Keys that are
deliberately not consumed yet must be listed in PENDING_CONSUMERS with a reason,
which keeps a known gap visible rather than either silently dead or silently
"fixed" by deleting a setting someone still intends to wire up.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - matches check-kb-container-packaging.py
    yaml = None

EMITTER = Path("src/modules/config/config_database.c")

# Emitted keys with no consumer yet, and why that is currently acceptable. Removing
# an entry from here is the goal; adding one needs a reason that names the work.
PENDING_CONSUMERS = {
    "AIMEE_LLM_SYNTH_MODE": (
        "local-synth selection. The aimee-llm container that consumed this is "
        "retired and no local synth backend has replaced it yet, so nothing reads "
        "it. Wire up or drop with the synth work."
    ),
    "AIMEE_LLM_SYNTH_TIER": (
        "local-synth tier (cpu/gpu). Same as AIMEE_LLM_SYNTH_MODE: the consumer was "
        "the retired gateway. Wire up or drop with the synth work."
    ),
    # COMPOSE_PROFILES is read by docker compose itself, not by this repo.
    "COMPOSE_PROFILES": "consumed by docker compose, not by any file in this tree.",
}


def tracked(root: Path, *globs: str) -> list[Path]:
    """Repo-relative tracked paths. git runs in root, not the caller's cwd — the
    Makefile invokes this from src/ with --root .., and resolving paths against the
    wrong directory silently found nothing and reported every key as unconsumed."""
    out = subprocess.run(
        ["git", "ls-files", *globs], capture_output=True, text=True, check=True, cwd=root
    )
    return [Path(p) for p in out.stdout.split() if p]


def emitted_keys(root: Path) -> list[str]:
    text = (root / EMITTER).read_text(encoding="utf-8")
    return sorted(set(re.findall(r'EMITF\("([A-Z_][A-Z0-9_]*)=', text)))


def consumers(root: Path, key: str) -> list[str]:
    """Where key is READ. Setting it is not consuming it.

    Reads look like getenv("KEY") / runtime_secret_get("KEY" in C, ${KEY} or $KEY in
    shell and Compose interpolation. Assignments (KEY=value, --env KEY=..., setenv)
    are deliberately NOT matched: the whole point is that emitting a value is not the
    same as anything using it.
    """
    reads = [
        re.compile(r'getenv\s*\(\s*"%s"' % re.escape(key)),
        re.compile(r'runtime_secret_get\s*\(\s*"%s"' % re.escape(key)),
        re.compile(r"\$\{%s[:\-}]" % re.escape(key)),
        re.compile(r"\$%s\b" % re.escape(key)),
    ]
    hits: list[str] = []
    emitter_rel = EMITTER.as_posix()
    for path in tracked(root, "*.c", "*.h", "*.sh", "*.py", "*.yaml", "*.yml"):
        rel = path.as_posix()
        if rel == emitter_rel or rel.endswith("src/tests/test_config.c"):
            continue
        if rel == "scripts/check-deploy-env-consumed.py":
            continue
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if key not in text:
            continue
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith(("*", "//", "#", "/*")):
                continue
            if any(r.search(line) for r in reads):
                hits.append(f"{rel}: {stripped[:100]}")
                break
        if hits:
            break
    return hits


# Files that run INSIDE the aimee-kb container and read the deployed environment.
# A key both emitted by the deploy layer and read here must actually be handed to the
# aimee-kb service, or it never reaches the process that reads it.
KB_RUNTIME_READERS = ("deploy/container/aimee-kb-entrypoint.sh",)

# Every Compose file that defines an aimee-kb service the deploy layer can start.
KB_COMPOSE_FILES = (
    "compose.yaml",
    "compose.server.yaml",
    "deploy/compose/aimee.yaml",
    "deploy/smoothnas/aimee.compose.yaml",
    "deploy/container/aimee-managed.compose.yaml",
)


def kb_read_keys(root: Path, emitted: list[str]) -> list[str]:
    """Emitted keys that something inside the kb container reads."""
    text = ""
    for rel in KB_RUNTIME_READERS:
        try:
            text += (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
    out = []
    for key in emitted:
        if re.search(r"\$\{?%s\b" % re.escape(key), text):
            out.append(key)
    return out


def kb_env_failures(root: Path, emitted: list[str]) -> list[str]:
    """The rule that catches an emitted-but-unplumbed key.

    "Consumed somewhere in the tree" is too weak: EMBEDDER_MODEL was read by the kb
    entrypoint AND by the legacy embedder service's own environment, so a
    tree-wide search found it while the aimee-kb service never received it. The kb
    entrypoint read a variable Compose never passed in, saw no selection, and started
    nothing. So check the specific thing: the service that runs the reader must be
    given the key.
    """
    if yaml is None:
        return ["PyYAML is required to validate the aimee-kb service environment"]

    failures: list[str] = []
    needed = kb_read_keys(root, emitted)
    for rel in KB_COMPOSE_FILES:
        path = root / rel
        if not path.exists():
            failures.append(f"{rel} is missing — it defines a deployable aimee-kb")
            continue
        try:
            model = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:
            failures.append(f"{rel}: invalid YAML ({exc.__class__.__name__})")
            continue
        services = (model or {}).get("services") or {}
        kb = services.get("aimee-kb")
        if not isinstance(kb, dict):
            failures.append(f"{rel}: no aimee-kb service")
            continue
        env = kb.get("environment")
        keys = set(env.keys()) if isinstance(env, dict) else set()
        for key in needed:
            if key not in keys:
                failures.append(
                    f"{rel}: aimee-kb never receives {key}, but the deploy layer emits "
                    f"it and something inside the container reads it — the setting "
                    f"cannot reach the process that needs it"
                )
    return failures


def check(root: Path) -> list[str]:
    failures: list[str] = []
    keys = emitted_keys(root)
    if not keys:
        return ["found no EMITF keys — has the emitter moved?"]
    failures.extend(kb_env_failures(root, keys))
    for key in keys:
        if consumers(root, key):
            continue
        if key in PENDING_CONSUMERS:
            continue
        failures.append(
            f"{key} is emitted by config_emit_deploy_env but nothing reads it — a "
            f"setting the user configures that silently does nothing. Consume it "
            f"(for the kb, add it to the aimee-kb service environment in the "
            f"shipped Compose files) or record it in PENDING_CONSUMERS with a reason"
        )
    return failures


def plant_test(root: Path) -> int:
    """A key with no consumer and no PENDING entry must be reported."""
    keys = emitted_keys(root)
    bogus = "AIMEE_DEPLOY_ENV_PLANTED_KEY"
    assert bogus not in keys
    text = (root / EMITTER).read_text(encoding="utf-8")
    planted = text.replace(
        'EMITF("COMPOSE_PROFILES=%s\\n", profiles);',
        'EMITF("COMPOSE_PROFILES=%s\\n", profiles);\n   EMITF("'
        + bogus
        + '=%s\\n", "x");',
        1,
    )
    if planted == text:
        print("check-deploy-env-consumed plant: could not plant a key", file=sys.stderr)
        return 1
    original = text
    try:
        (root / EMITTER).write_text(planted, encoding="utf-8")
        failures = check(root)
    finally:
        (root / EMITTER).write_text(original, encoding="utf-8")
    if not any(bogus in f for f in failures):
        print(
            "check-deploy-env-consumed plant: an unconsumed key was NOT reported",
            file=sys.stderr,
        )
        return 1
    print("check-deploy-env-consumed: plant-test ok (an unconsumed key is named)")
    return 0


def main() -> int:
    root = Path(".").resolve()
    args = sys.argv[1:]
    if "--root" in args:
        root = Path(args[args.index("--root") + 1]).resolve()
    if "--plant-test" in args:
        return plant_test(root)
    failures = check(root)
    if failures:
        print("check-deploy-env-consumed: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    keys = emitted_keys(root)
    pending = len(PENDING_CONSUMERS)
    print(
        f"check-deploy-env-consumed: ok ({len(keys)} emitted key(s) all consumed or "
        f"recorded; {pending} awaiting a consumer)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
