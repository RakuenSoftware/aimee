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

EMITTER = Path("src/config_client_contract.c")

# Emitted keys with no consumer yet, and why that is currently acceptable. Removing
# an entry from here is the goal; adding one needs a reason that names the work.
PENDING_CONSUMERS = {
    # AIMEE_LLM_SYNTH_MODE and AIMEE_LLM_SYNTH_TIER were listed here as awaiting a
    # consumer after the aimee-llm gateway was retired. They are now DELETED rather
    # than pending: the emitter no longer writes them, and a bundled model is
    # selected by SYNTHESIS_MODEL on a *-llm image, which the kb entrypoint reads.
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
    return sorted(set(re.findall(r'"([A-Z_][A-Z0-9_]*)=', text)))


# Emitted keys whose only legitimate reader IS a Compose file, with the reason. An
# image tag is chosen by Compose interpolation; there is no process to read it.
# Everything else needs a reader in code, see code_consumers().
COMPOSE_ONLY_KEYS = {
    "AIMEE_KB_VARIANT": "selects the aimee-kb image tag; consumed by Compose interpolation.",
    "AIMEE_LLM_VARIANT": "selects the aimee-llm image tag; consumed by Compose interpolation.",
}

# Extensions that hold something that RUNS. A Compose file is plumbing: it can hand a
# variable to a container and prove nothing about anybody reading it.
CODE_GLOBS = ("*.c", "*.h", "*.sh", "*.py")


def code_consumers(root: Path, key: str) -> list[str]:
    """Where key is read by code that runs, as opposed to YAML that forwards it.

    THE DISTINCTION IS NOT PEDANTIC. SYNTHESIS_CA_FILE, SYNTHESIS_CERT_FILE and
    SYNTHESIS_KEY_FILE were emitted by the deploy layer, listed in the aimee-kb
    service environment, and delivered to the container -- and no line of C ever
    called getenv on any of them. This check passed the whole time, because a
    `${SYNTHESIS_CA_FILE:-}` in a Compose file matched its "is it consumed" regex.

    What that bought: every synthesis call from the kb used the default TLS context,
    the sidecar's certificate chained to a CA the client did not know, and the
    handshake failed with "tlsv1 alert unknown ca". The operator saw a curator job
    stuck at `provider HTTP -1`. The mTLS hop had a passing end-to-end test the whole
    time -- it drove curl with the certificates, never the kb's own client.

    So: forwarding is not consumption, and only a reader in something executable
    counts.
    """
    return _search(root, key, CODE_GLOBS)


def consumers(root: Path, key: str) -> list[str]:
    """Where key is READ. Setting it is not consuming it.

    Reads look like getenv("KEY") / runtime_secret_get("KEY" in C, ${KEY} or $KEY in
    shell and Compose interpolation. Assignments (KEY=value, --env KEY=..., setenv)
    are deliberately NOT matched: the whole point is that emitting a value is not the
    same as anything using it.
    """
    return _search(root, key, ("*.c", "*.h", "*.sh", "*.py", "*.yaml", "*.yml"))


def _search(root: Path, key: str, globs: tuple[str, ...]) -> list[str]:
    reads = [
        re.compile(r'getenv\s*\(\s*"%s"' % re.escape(key)),
        re.compile(r'runtime_secret_get\s*\(\s*"%s"' % re.escape(key)),
        re.compile(r"\$\{%s[:\-}]" % re.escape(key)),
        re.compile(r"\$%s\b" % re.escape(key)),
    ]
    hits: list[str] = []
    emitter_rel = EMITTER.as_posix()
    for path in tracked(root, *globs):
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


# The Compose file that starts aimee-server itself, and the service within it. The
# server re-runs Compose for the managed siblings and copies its own environ, so
# anything set here is inherited by that child.
SERVER_COMPOSE = "compose.server-managed.yaml"
SERVER_SERVICE = "aimee-server"


def server_env_shadow_failures(root: Path, emitted: list[str]) -> list[str]:
    """The rule that catches an emitted key OVERRIDDEN before it can be read.

    Consumed-somewhere is not enough, and neither is reaching the container. A key
    can be emitted, plumbed, read — and still lose, because a DIFFERENT variable
    forwarded into aimee-server's own environment already answers the question.

    That is what AIMEE_KB_IMAGE did. compose.server-managed.yaml set

        AIMEE_KB_IMAGE: ${AIMEE_KB_IMAGE:-...aimee-kb${AIMEE_KB_VARIANT:+-${AIMEE_KB_VARIANT}}:...}

    and AIMEE_KB_VARIANT is decided INSIDE the server by config_emit_deploy_env, from
    the saved wizard configuration. It is unset in the shell that brings the server
    up, so this collapsed to a bare `aimee-kb:<tag>` and baked it into the server's
    environment. deploy_apply copies environ, so the managed compose's own
    variant-aware default was pre-empted by the explicit value and the wizard's
    embedder choice could not change the image. Selecting bekko-a25m deployed the
    embedderless kb, which serves lexical-only search and says so in one log line
    nobody reads. Nothing failed: not the deploy, not the healthcheck, not `kb smoke`.

    So: no variable forwarded to aimee-server may resolve a default that interpolates
    a key the deploy layer emits. Forward it empty (`${KEY:-}`) and let the managed
    Compose file, which runs with the emitted value in its environment, resolve it.
    """
    if yaml is None:
        return ["PyYAML is required to validate the aimee-server service environment"]
    path = root / SERVER_COMPOSE
    if not path.exists():
        return [f"{SERVER_COMPOSE} is missing — it starts the server that re-runs Compose"]
    try:
        model = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        return [f"{SERVER_COMPOSE}: invalid YAML ({exc.__class__.__name__})"]
    service = ((model or {}).get("services") or {}).get(SERVER_SERVICE)
    if not isinstance(service, dict):
        return [f"{SERVER_COMPOSE}: no {SERVER_SERVICE} service"]
    env = service.get("environment")
    if not isinstance(env, dict):
        return [f"{SERVER_COMPOSE}: {SERVER_SERVICE} has no mapping-form environment"]

    failures: list[str] = []
    for name, value in env.items():
        if not isinstance(value, str):
            continue
        # Only the DEFAULT half can shadow: `${NAME:-<default>}`. A bare `${NAME}`
        # forwards whatever the operator set and resolves nothing on its own.
        default = re.match(r"^\$\{%s:-(.*)\}$" % re.escape(str(name)), value.strip())
        if not default:
            continue
        for key in emitted:
            if key == name:
                continue
            if re.search(r"\$\{?%s\b" % re.escape(key), default.group(1)):
                failures.append(
                    f"{SERVER_COMPOSE}: {SERVER_SERVICE} resolves a default for {name} "
                    f"that reads {key}, which config_emit_deploy_env decides inside the "
                    f"server and which is unset here — the default collapses and then "
                    f"overrides the value the server computes. Forward it as "
                    f"${{{name}:-}} and let the managed Compose file resolve it"
                )
    return failures


def check(root: Path) -> list[str]:
    failures: list[str] = []
    keys = emitted_keys(root)
    if not keys:
        return ["found no EMITF keys — has the emitter moved?"]
    failures.extend(kb_env_failures(root, keys))
    failures.extend(server_env_shadow_failures(root, keys))
    for key in keys:
        if key in PENDING_CONSUMERS or key in COMPOSE_ONLY_KEYS:
            continue
        if not consumers(root, key):
            failures.append(
                f"{key} is emitted by config_emit_deploy_env but nothing reads it — a "
                f"setting the user configures that silently does nothing. Consume it "
                f"(for the kb, add it to the aimee-kb service environment in the "
                f"shipped Compose files) or record it in PENDING_CONSUMERS with a reason"
            )
            continue
        if not code_consumers(root, key):
            failures.append(
                f"{key} is emitted and forwarded by a Compose file, but no code reads "
                f"it — getenv/runtime_secret_get in C, or $VAR in a shell script. "
                f"Handing a variable to a container proves nothing about anybody using "
                f"it: this is exactly how SYNTHESIS_CA_FILE reached the kb and was "
                f"ignored, leaving every synthesis call to fail the TLS handshake. "
                f"Read it, or record it in COMPOSE_ONLY_KEYS with a reason"
            )
    return failures


def plant_test(root: Path) -> int:
    """A key with no consumer and no PENDING entry must be reported."""
    keys = emitted_keys(root)
    bogus = "AIMEE_DEPLOY_ENV_PLANTED_KEY"
    assert bogus not in keys
    text = (root / EMITTER).read_text(encoding="utf-8")
    # Plant beside the first emitter call instead of coupling the proof to a
    # particular deploy key. The external module now owns the set and ordering
    # of emitted keys, so a key-specific anchor would make this check fail when
    # that perfectly valid implementation detail changes.
    planted = re.sub(
        r'(\bEMITF\([^;]+;)',
        r'\1\n   EMITF("' + bogus + r'=%s\\n", "x");',
        text,
        count=1,
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
