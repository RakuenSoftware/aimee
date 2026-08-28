#!/usr/bin/env python3
"""Check static packaging invariants for the aimee-kb container."""

from __future__ import annotations

import argparse
import posixpath
import re
import sys
import tempfile
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - fail-closed packaging prerequisite
    yaml = None


REQUIRED_DOCKERFILE_PATTERNS = {
    "named-build-stage": r"(?im)^FROM\s+\S+\s+AS\s+build\b",
    "builds-aimee-kb": r"make\s+-C\s+src\s+\.\./aimee-kb",
    "builds-aimee-kb-worm": r"make\s+-C\s+src[^\n]*\.\./aimee-kb-worm",
    "copies-aimee-kb": r"COPY\s+--from=build\s+/src/aimee-kb\s+/usr/local/bin/aimee-kb",
    "copies-aimee-kb-worm": (
        r"COPY\s+--from=build\s+/src/aimee-kb-worm\s+/usr/local/bin/aimee-kb-worm"
    ),
    # SQLite belongs only to the separately linked WORM worker. check-linking
    # independently rejects SQLite linkage in the aimee-kb service binary.
    "sqlite-worm-build-dependency": r"\blibsqlite3-dev\b",
    "sqlite-worm-runtime-dependency": r"\blibsqlite3-0\b",
    "aimee-home-env": r"(?m)^ENV\s+AIMEE_HOME=/var/lib/aimee\b",
    # DB2 ships IN the image so an unconfigured deployment has a working vector
    # store without pulling a third-party database image at runtime.
    "db2-embedded-engine": r"postgresql-\$\{PG_MAJOR\}-pgvector",
    "runtime-user": r"(?m)^USER\s+aimee\b",
    "health-v1": r"HEALTHCHECK[\s\S]*/v1/health",
    "exposes-http": r"EXPOSE\s+8741",
    "entrypoint-kb": r"ENTRYPOINT\s+\[[^\]]*aimee-kb[^\]]*\]",
    # The LLM/embedder access code the kb popens must ship in the image, plus a
    # python3 to run them and a default config selecting the sidecar commands.
    "python3-runtime": r"(?m)^\s+python3\b",
    "copies-sidecars": r"(?s)COPY\s+scripts/embed-remote\.py.*?/opt/aimee/scripts",
    "copies-config": r"COPY\b[^\n]*deploy/container/aimee\.yaml",
}

FORBIDDEN_DOCKERFILE_PATTERNS = {
    "server-binary": r"aimee-server",
    "db1-reference": r"\bDB1\b|db1/",
    # A baked URL makes an unconfigured install silently depend on a sibling
    # container instead of starting the PostgreSQL shipped inside aimee-kb.
    "db2-url-pinned": r"(?m)^ENV\s+AIMEE_DB2_URL=",
}

REQUIRED_COMPOSE_PATTERNS = {
    "kb-service": r"(?m)^\s{2}aimee-kb:",
    "kb-build-context": r"(?s)aimee-kb:.*build:.*context:\s*\.",
    "aimee-home-env": r"(?s)aimee-kb:.*AIMEE_HOME:\s*/var/lib/aimee",
    "kb-health": r"(?s)aimee-kb:.*healthcheck:.*http://127\.0\.0\.1:8741/v1/health",
    # SYNTHESIS_ENDPOINT is SYNTH ONLY now and must carry no default — the container it used
    # to name is gone, so a default would point every deploy at a dead host.
    "llm-url-no-default": r"(?m)^\s*SYNTHESIS_ENDPOINT:\s*\$\{SYNTHESIS_ENDPOINT:-\}\s*$",
}

FORBIDDEN_COMPOSE_PATTERNS = {
    "kb-port-non-loopback": r'(?m)^\s*-\s*["\']?8741:8741["\']?\s*$',
    "standalone-postgres-service": r"(?m)^\s{2}postgres:",
}

SERVER_IDENTITY_ENV = {
    "AIMEE_SERVER_ID": "${AIMEE_SERVER_ID:-}",
    "AIMEE_SERVER_TEAM_ID": "${AIMEE_SERVER_TEAM_ID:-}",
    "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE": (
        "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-"
        "/run/aimee/management/jwks-trust-bundle.json}"
    ),
}
SERVER_MANAGEMENT_MOUNT = (
    "${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro"
)
SERVER_MANAGED_TRUST_ENV = (
    "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-"
    "/run/aimee/managed-trust/jwks-trust-bundle.json}"
)
SERVER_MANAGED_TRUST_MOUNT = "aimee-managed-jwks-trust:/run/aimee/managed-trust:ro"
KB_MTLS_ENV = {
    "AIMEE_KB_MTLS_HOST": "aimee-kb",
    "AIMEE_KB_MTLS_PORT": "8745",
}
CONTROL_WEB_CRED_MOUNT = (
    "${CONTROL_WEB_CRED_DIR:-./control-web-secrets}:/run/control-web:ro"
)

REQUIRED_DOCKERIGNORE_ENTRIES = {
    ".git",
    ".aimee",
    ".env",
    "build",
    "src/build",
    "frontend/node_modules",
    # Exclude stale host binaries from the build context. This does not hide the
    # multi-stage builder's /src/aimee-kb output, which is created inside the image.
    "/aimee",
    "/aimee-server",
    "/aimee-kb",
    "/aimee-runtime-web",
    "/aimee-gateway",
}

FORBIDDEN_DOCKERIGNORE_ENTRIES = {
    "Dockerfile",
    "compose.yaml",
    "src",
    "src/**",
    "scripts",
    "scripts/**",
}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def normalized_dockerignore(path: Path) -> set[str]:
    entries: set[str] = set()
    for raw in read(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        negated = line.startswith("!")
        value = line[1:] if negated else line
        rooted = value.startswith("/")
        value = posixpath.normpath(value.rstrip("/"))
        if value == ".":
            continue
        if rooted and not value.startswith("/"):
            value = "/" + value
        entries.add(("!" if negated else "") + value)
    return entries


def missing_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if not re.search(pattern, text)]


def present_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if re.search(pattern, text)]


def has_compose_interpolation(value: object) -> bool:
    """Return true for Compose $VAR/${VAR}; $$ is a literal dollar escape."""
    text = str(value)
    index = 0
    while index < len(text):
        if text[index] != "$":
            index += 1
            continue
        if index + 1 < len(text) and text[index + 1] == "$":
            index += 2
            continue
        if index + 1 < len(text) and (text[index + 1] == "{" or re.match(r"[A-Za-z_]", text[index + 1])):
            return True
        index += 1
    return False


if yaml is not None:
    class UniqueKeySafeLoader(yaml.SafeLoader):
        """SafeLoader that rejects duplicate lexical mapping keys before merge resolution."""


    def _construct_unique_mapping(loader: UniqueKeySafeLoader, node: yaml.MappingNode, deep: bool = False):
        seen: set[tuple[str, str]] = set()
        for key_node, _ in node.value:
            if str(key_node.value) == "<<" or key_node.tag == "tag:yaml.org,2002:merge":
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    "YAML merge keys are unsupported by this fail-closed packaging gate",
                    key_node.start_mark,
                )
            identity = (key_node.tag, str(key_node.value))
            if identity in seen:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    f"duplicate mapping key {key_node.value!r}",
                    key_node.start_mark,
                )
            seen.add(identity)
        return yaml.SafeLoader.construct_mapping(loader, node, deep=deep)


    UniqueKeySafeLoader.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_unique_mapping
    )


def kb_publication_failures(text: str) -> list[str]:
    """Validate the parsed effective KB service; reject unresolved Compose features.

    YAML aliases may name scalar/list values, but merge keys, Compose interpolation, and `extends`
    are rejected because this static gate cannot prove their effective value. The accepted port
    grammar is deliberately closed: exactly one quoted short-form entry, or exactly one Compose
    target/published/host_ip entry with an optional explicit TCP protocol.
    """
    if yaml is None:
        return ["PyYAML is required to validate the effective Compose model"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    service = services.get("aimee-kb") if isinstance(services, dict) else None
    if not isinstance(service, dict):
        return ["missing aimee-kb service"]
    failures: list[str] = []
    required_services = {"aimee-kb"}
    missing_services = required_services - set(services)
    if missing_services:
        failures.append("missing required services: " + ", ".join(sorted(missing_services)))
    # The retired container must not return by accident: the kb embeds itself, and a
    # resurrected service would race it for the embedding role.
    if "aimee-llm" in services:
        failures.append("aimee-llm is retired; the kb embeds in-container")
    build = service.get("build")
    if not isinstance(build, dict) or build.get("context") != ".":
        failures.append("aimee-kb build context must be exactly '.'")
    if isinstance(build, dict) and build.get("dockerfile", "Dockerfile") != "Dockerfile":
        failures.append("aimee-kb build.dockerfile must reference the checked Dockerfile")
    environment = service.get("environment")
    if not isinstance(environment, dict):
        failures.append("aimee-kb environment must use parsed mapping form")
    else:
        if environment.get("AIMEE_HOME") != "/var/lib/aimee":
            failures.append("aimee-kb AIMEE_HOME must be exactly /var/lib/aimee")
        if "AIMEE_DB2_URL" in environment:
            failures.append("aimee-kb must load DB2 credentials from Vault, not Config.Env")
        if environment.get("SYNTHESIS_ENDPOINT") != "${SYNTHESIS_ENDPOINT:-}":
            failures.append(
                "aimee-kb SYNTHESIS_ENDPOINT must have an empty default: synthesis is "
                "optional, and whatever selects a provider supplies the endpoint"
            )
    depends_on = service.get("depends_on")
    if isinstance(depends_on, dict) and "postgres" in depends_on:
        failures.append("aimee-kb new-install topology must not depend on postgres")
    healthcheck = service.get("healthcheck")
    if "http://127.0.0.1:8741/v1/health" not in str(healthcheck):
        failures.append("aimee-kb healthcheck must use the loopback v1 health endpoint")
    if "extends" in service:
        failures.append("aimee-kb uses unresolved extends")
    network_mode = service.get("network_mode")
    if isinstance(network_mode, str) and has_compose_interpolation(network_mode):
        failures.append("aimee-kb uses interpolated network_mode")
    elif network_mode is not None:
        failures.append("aimee-kb network_mode must be omitted")

    ports = service.get("ports")
    if not isinstance(ports, list):
        return failures + ["aimee-kb must publish port 8741 with an explicit loopback entry"]
    if len(ports) != 1:
        failures.append("aimee-kb must publish exactly one canonical loopback port entry")
    saw_loopback_8741 = False
    allowed_keys = {"target", "published", "host_ip", "protocol"}
    for entry in ports:
        if isinstance(entry, str):
            if has_compose_interpolation(entry):
                failures.append("aimee-kb uses interpolated ports syntax")
                continue
            parts = entry.split("/", 1)
            value = parts[0].strip()
            protocol = parts[1] if len(parts) == 2 else "tcp"
            if "/" in protocol or protocol.lower() != "tcp":
                failures.append(f"aimee-kb KB port publication must use TCP: {entry}")
                continue
            accepted = re.fullmatch(r"(?:127\.0\.0\.1|\[::1\]):8741:8741", value)
            if accepted:
                saw_loopback_8741 = True
            else:
                failures.append(f"unsupported KB port publication {value}")
            continue
        if isinstance(entry, dict):
            if any(has_compose_interpolation(value) for value in entry.values()):
                failures.append("aimee-kb uses interpolated long-form ports syntax")
                continue
            unknown = set(entry) - allowed_keys
            if unknown:
                failures.append(
                    "unsupported long-form ports keys: " + ", ".join(sorted(str(k) for k in unknown))
                )
                continue
            target = str(entry.get("target", "")).strip()
            published = str(entry.get("published", "")).strip()
            if "-" in target or "-" in published:
                failures.append(
                    f"long-form port ranges are unsupported for aimee-kb: target={target} published={published}"
                )
                continue
            if not re.fullmatch(r"8741", target):
                failures.append("long-form KB target port must be the canonical integer 8741")
                continue
            if not re.fullmatch(r"8741", published):
                failures.append("long-form KB published port must be the canonical integer 8741")
                continue
            protocol = str(entry.get("protocol", "tcp")).strip().lower()
            if protocol != "tcp":
                failures.append("long-form KB port publication must use TCP")
                continue
            host_ip = str(entry.get("host_ip", "")).strip()
            if len(host_ip) >= 2 and host_ip.startswith("[") and host_ip.endswith("]"):
                host_ip = host_ip[1:-1].strip()
            if host_ip not in {"127.0.0.1", "::1"}:
                failures.append("long-form KB port publication lacks an explicit loopback host_ip")
            else:
                saw_loopback_8741 = True
            continue
        if entry == 8741:
            failures.append("bare KB target port publishes on all host interfaces")
        else:
            failures.append("unsupported non-scalar Compose ports entry")
    if not saw_loopback_8741:
        failures.append("aimee-kb has no effective loopback publication for target port 8741")
    return failures


def control_web_idle_health_failures(text: str) -> list[str]:
    """Keep optional control-web first boot consistent with Compose health.

    With no console credential, the process intentionally stays alive without a
    listener. Once a credential file exists, health must probe the real TLS SPA.
    """
    if yaml is None:
        return ["PyYAML is required to validate control-web Compose health"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    service = services.get("aimee-control-web") if isinstance(services, dict) else None
    if not isinstance(service, dict):
        return ["missing aimee-control-web service"]

    failures: list[str] = []
    environment = service.get("environment")
    if not isinstance(environment, dict):
        return ["aimee-control-web environment must use parsed mapping form"]
    if environment.get("AIMEE_CONTROL_WEB_ENABLED") != "${AIMEE_CONTROL_WEB_ENABLED:-1}":
        failures.append("aimee-control-web enable flag must be operator-overridable")
    if environment.get("CONTROL_WEB_CRED_FILE") != "/run/control-web/console.cred":
        failures.append("aimee-control-web credential path changed unexpectedly")
    volumes = service.get("volumes")
    if not isinstance(volumes, list) or CONTROL_WEB_CRED_MOUNT not in volumes:
        failures.append("aimee-control-web must mount the credential parent directory read-only")

    healthcheck = service.get("healthcheck")
    test = healthcheck.get("test") if isinstance(healthcheck, dict) else None
    if not isinstance(test, list) or len(test) != 2 or test[0] != "CMD-SHELL":
        failures.append("aimee-control-web healthcheck must use one bounded shell decision")
        return failures
    command = str(test[1])
    required = {
        "disabled-state handling": "AIMEE_CONTROL_WEB_ENABLED",
        "credential-file handling": 'if [ ! -f "$${CONTROL_WEB_CRED_FILE}" ]',
        "live TLS probe": "curl -fsSk https://127.0.0.1:8744/",
    }
    for name, marker in required.items():
        if marker not in command:
            failures.append(f"aimee-control-web healthcheck missing {name}")
    return failures


def server_identity_failures(text: str, managed: bool = False) -> list[str]:
    """Validate the opt-in server identity contract in a release Compose file.

    Empty server/team/enrollment values preserve read-only startup. The public
    trust roots have a stable in-container default and a read-only host mount so
    an operator can enable writes without editing a shipped manifest.
    """
    if yaml is None:
        return ["PyYAML is required to validate the effective Compose model"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    service = services.get("aimee-server") if isinstance(services, dict) else None
    if not isinstance(service, dict):
        return ["missing aimee-server service"]
    failures: list[str] = []
    environment = service.get("environment")
    if not isinstance(environment, dict):
        return ["aimee-server environment must use parsed mapping form"]
    expected_env = dict(SERVER_IDENTITY_ENV)
    if managed:
        expected_env["AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE"] = SERVER_MANAGED_TRUST_ENV
    for name, expected in expected_env.items():
        if environment.get(name) != expected:
            failures.append(f"aimee-server {name} must pass through as {expected}")
    volumes = service.get("volumes")
    if not isinstance(volumes, list) or SERVER_MANAGEMENT_MOUNT not in volumes:
        failures.append("aimee-server missing the read-only management trust mount")
    if managed and (
        not isinstance(volumes, list) or SERVER_MANAGED_TRUST_MOUNT not in volumes
    ):
        failures.append("aimee-server missing the read-only wizard-managed trust volume")
    return failures


def kb_mtls_failures(text: str) -> list[str]:
    """Require the private enrollment/JWKS listener in managed KB topologies."""
    if yaml is None:
        return ["PyYAML is required to validate the effective Compose model"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    service = services.get("aimee-kb") if isinstance(services, dict) else None
    if not isinstance(service, dict):
        return ["missing aimee-kb service"]
    environment = service.get("environment")
    if not isinstance(environment, dict):
        return ["aimee-kb environment must use parsed mapping form"]
    return [
        f"aimee-kb {name} must be exactly {expected}"
        for name, expected in KB_MTLS_ENV.items()
        if str(environment.get(name, "")) != expected
    ]


def server_default_config_failures(text: str) -> list[str]:
    if yaml is None:
        return ["PyYAML is required to validate the server container defaults"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid server default YAML: {exc.__class__.__name__}"]
    api = model.get("aimee", {}).get("api") if isinstance(model, dict) else None
    if not isinstance(api, dict) or api.get("remote_writes") != "off":
        return ["server container default remote_writes must be the inert value 'off'"]
    return []


def managed_kb_llm_contract_failures(text: str) -> list[str]:
    """Validate the wizard-managed KB-to-LLM identity and role contract."""
    if yaml is None:
        return ["PyYAML is required to validate the managed KB-to-LLM contract"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid managed Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    kb = services.get("aimee-kb") if isinstance(services, dict) else None
    if not isinstance(kb, dict):
        return ["managed Compose must contain the aimee-kb service"]
    failures: list[str] = []

    # aimee-llm is allowed again, but only in the shape that makes it safe to
    # deploy. The old one was retired for coupling synthesis to the kb image and
    # for carrying the embedder role; each check below is one of those ways back.
    llm = services.get("aimee-llm") if isinstance(services, dict) else None
    if isinstance(llm, dict):
        if llm.get("profiles") != ["llm"]:
            failures.append(
                "aimee-llm must be gated behind the 'llm' profile; an ungated "
                "sidecar deploys on every managed stack, including those using an "
                "external synthesis provider or none"
            )
        llm_env = llm.get("environment")
        if isinstance(llm_env, dict) and any(k.startswith("EMBEDDER") for k in llm_env):
            failures.append(
                "aimee-llm must not carry an embedder role; the kb embeds "
                "in-container and a second embedder is the topology that was retired"
            )
        volumes = llm.get("volumes")
        if not (
            isinstance(volumes, list)
            and any(
                isinstance(v, str) and "synthesis-tls" in v and v.endswith(":ro")
                for v in volumes
            )
        ):
            failures.append(
                "aimee-llm must mount the synthesis identity READ-ONLY; it presents "
                "that material and never issues any"
            )
        llm_dep = llm.get("depends_on")
        healthy = (
            isinstance(llm_dep, dict)
            and isinstance(llm_dep.get("aimee-kb"), dict)
            and llm_dep["aimee-kb"].get("condition") == "service_healthy"
        )
        if not healthy:
            failures.append(
                "aimee-llm must depend on aimee-kb being service_healthy: the kb "
                "mints the mTLS identity the sidecar refuses to start without"
            )

    kb_env = kb.get("environment")
    if not isinstance(kb_env, dict):
        return ["managed KB environment must use mapping form"]

    token_expr = "${SYNTHESIS_API_KEY:-}"
    if "SYNTHESIS_API_KEY" in kb_env:
        failures.append("managed KB LLM bearer must be loaded from Vault, not Config.Env")
    if kb_env.get("SYNTHESIS_AUTH_REQUIRED") != "${SYNTHESIS_AUTH_REQUIRED:-0}":
        failures.append(
            "managed KB auth-required mode must follow the local-LLM deployment transaction"
        )
    if "SYNTHESIS_API_KEY" in kb_env:
        failures.append("managed KB legacy curator token must not be stored in Config.Env")
    if kb_env.get("SYNTHESIS_ENDPOINT") != "${SYNTHESIS_ENDPOINT:-}":
        failures.append("managed KB SYNTHESIS_ENDPOINT must have an empty default (synth only)")
    for key in kb_env:
        if str(key).startswith("AIMEE_LLM_EMBED_"):
            failures.append(f"managed KB must not receive {key}; the embedder is in-container")
    if kb_env.get("SYNTHESIS_MODEL") != "${SYNTHESIS_MODEL:-aimee-synth}":
        failures.append("managed KB must receive the unified model label")

    depends_on = kb.get("depends_on")
    if (isinstance(depends_on, dict) and "aimee-llm" in depends_on) or (
        isinstance(depends_on, list) and "aimee-llm" in depends_on
    ):
        failures.append("managed KB must not depend on the retired aimee-llm service")
    return failures


# Every Compose file this repo ships. Structural mistakes must be caught in all of
# them, not just the two the packaging rules inspect in detail.
SHIPPED_COMPOSE_FILES = (
    "compose.yaml",
    "compose.server.yaml",
    "compose.server-managed.yaml",
    "compose.server-standalone.yaml",
    "deploy/compose/aimee.yaml",
    "deploy/container/aimee-managed.compose.yaml",
    "deploy/container/aimee-server.yaml",
    "deploy/smoothnas/aimee.compose.yaml",
)


def empty_key_failures(text: str) -> list[str]:
    """Catch a service key that is present but null.

    Deleting the last entry under a block key (`depends_on:` losing its only
    service, say) leaves the key behind holding None. That is still valid YAML,
    so a parse test passes — but Compose rejects it ("depends_on must be a
    array") and the whole topology fails to start. `docker compose config` finds
    it; this repo's lint has no docker, so the shape is asserted directly.
    """
    if yaml is None:
        return ["PyYAML is required to validate Compose service shape"]
    try:
        model = yaml.load(text, Loader=UniqueKeySafeLoader)
    except yaml.YAMLError as exc:
        return [f"invalid Compose YAML: {exc.__class__.__name__}"]
    services = model.get("services") if isinstance(model, dict) else None
    if not isinstance(services, dict):
        return []
    failures: list[str] = []
    for name, service in services.items():
        if not isinstance(service, dict):
            continue
        for key, value in service.items():
            if value is None:
                failures.append(
                    f"{name}.{key} is present but empty — remove the key or give it a value"
                )
    return failures


def check(root: Path) -> list[str]:
    failures: list[str] = []
    for rel in SHIPPED_COMPOSE_FILES:
        path = root / rel
        if path.exists():
            for failure in empty_key_failures(read(path)):
                failures.append(f"{rel} {failure}")
    dockerfile = root / "Dockerfile"
    compose = root / "compose.yaml"
    server_compose = root / "compose.server.yaml"
    dockerignore = root / ".dockerignore"
    compose_env = root / ".env"

    if compose_env.exists() and re.search(
        r"(?m)^\s*COMPOSE_PROFILES\s*=\s*\S+", read(compose_env)
    ):
        failures.append(".env enables optional Compose profiles by default")

    server_dockerfile = root / "Dockerfile.server"
    server_defaults = root / "deploy" / "container" / "aimee-server-remote-writes.yaml"
    server_tls = root / "src" / "server" / "server_tls.c"
    if not server_dockerfile.exists() or "ENV AIMEE_HOME=/var/lib/aimee" not in read(
        server_dockerfile
    ):
        failures.append("server image missing AIMEE_HOME=/var/lib/aimee certificate root")
    if not server_defaults.exists():
        failures.append("missing server container default configuration")
    else:
        failures.extend(server_default_config_failures(read(server_defaults)))
    if not server_tls.exists() or not all(
        marker in read(server_tls)
        for marker in ('"%s/tls/server.crt"', "config_default_dir()", "pki_ensure_self_signed_server_cert")
    ):
        failures.append("server TLS code missing the $AIMEE_HOME/tls/server.crt provisioning contract")
    # These are all release artifacts in this repository, not optional inputs to
    # a reusable library checker; absence of any one is a packaging regression.
    for topology in (
        "compose.server-managed.yaml",
        "compose.server.yaml",
        "compose.server-standalone.yaml",
    ):
        path = root / topology
        if not path.exists() or not re.search(r"(?m)^  aimee-server:\s*$", read(path)):
            failures.append(f"{topology} missing aimee-server certificate-bearing service")
        elif path.exists():
            for failure in server_identity_failures(
                read(path), managed=topology == "compose.server-managed.yaml"
            ):
                failures.append(f"{topology} {failure}")

    if not dockerfile.exists():
        failures.append("missing Dockerfile")
    else:
        text = read(dockerfile)
        for name in missing_patterns(text, REQUIRED_DOCKERFILE_PATTERNS):
            failures.append(f"Dockerfile missing {name}")
        for name in present_patterns(text, FORBIDDEN_DOCKERFILE_PATTERNS):
            failures.append(f"Dockerfile contains forbidden {name}")

    if not compose.exists():
        failures.append("missing compose.yaml")
    else:
        text = read(compose)
        for name in missing_patterns(text, REQUIRED_COMPOSE_PATTERNS):
            failures.append(f"compose.yaml missing {name}")
        for name in present_patterns(text, FORBIDDEN_COMPOSE_PATTERNS):
            failures.append(f"compose.yaml contains forbidden {name}")
        for failure in kb_publication_failures(text):
            failures.append(f"compose.yaml {failure}")
        for failure in control_web_idle_health_failures(text):
            failures.append(f"compose.yaml {failure}")

    if not server_compose.exists():
        failures.append("missing compose.server.yaml")
    else:
        for failure in kb_publication_failures(read(server_compose)):
            failures.append(f"compose.server.yaml {failure}")
        for failure in kb_mtls_failures(read(server_compose)):
            failures.append(f"compose.server.yaml {failure}")

    managed_compose = root / "deploy" / "container" / "aimee-managed.compose.yaml"
    if not managed_compose.exists():
        failures.append("missing deploy/container/aimee-managed.compose.yaml")
    else:
        managed_text = read(managed_compose)
        for failure in kb_mtls_failures(managed_text):
            failures.append(f"deploy/container/aimee-managed.compose.yaml {failure}")
        for failure in managed_kb_llm_contract_failures(managed_text):
            failures.append(f"deploy/container/aimee-managed.compose.yaml {failure}")

    if not dockerignore.exists():
        failures.append("missing .dockerignore")
    else:
        entries = normalized_dockerignore(dockerignore)
        for entry in sorted(REQUIRED_DOCKERIGNORE_ENTRIES - entries):
            failures.append(f".dockerignore missing {entry}")
        hidden_inputs = {entry.lstrip("/") for entry in entries if not entry.startswith("!")}
        for entry in sorted(FORBIDDEN_DOCKERIGNORE_ENTRIES & hidden_inputs):
            failures.append(f".dockerignore hides required build input {entry}")

    return failures


def plant_test() -> int:
    with tempfile.TemporaryDirectory(prefix="kb_container_packaging_") as tmp:
        root = Path(tmp)
        (root / "Dockerfile").write_text(
            "FROM debian\nRUN make -C src ../aimee-kb\nCOPY aimee-server /bad\n",
            encoding="utf-8",
        )
        (root / "compose.yaml").write_text("services:\n  aimee-kb:\n", encoding="utf-8")
        found = check(root)
        expected = {
            "Dockerfile missing named-build-stage",
            "Dockerfile missing copies-aimee-kb",
            "Dockerfile missing builds-aimee-kb-worm",
            "Dockerfile missing copies-aimee-kb-worm",
            "Dockerfile missing sqlite-worm-build-dependency",
            "Dockerfile missing sqlite-worm-runtime-dependency",
            "Dockerfile missing aimee-home-env",
            "Dockerfile missing db2-embedded-engine",
            "Dockerfile missing runtime-user",
            "Dockerfile missing health-v1",
            "Dockerfile missing exposes-http",
            "Dockerfile missing entrypoint-kb",
            "Dockerfile missing python3-runtime",
            "Dockerfile missing copies-sidecars",
            "Dockerfile missing copies-config",
            "Dockerfile contains forbidden server-binary",
            "compose.yaml missing kb-build-context",
            "compose.yaml missing aimee-home-env",
            "compose.yaml missing kb-health",
            "compose.yaml missing llm-url-no-default",
            "missing .dockerignore",
        }
        if not expected.issubset(set(found)):
            print("kb-container-packaging plant: failed", file=sys.stderr)
            for item in found:
                print(f"  found: {item}", file=sys.stderr)
            return 1

        (root / "Dockerfile").write_text(
            "\n".join(
                [
                    "FROM debian AS build",
                    "RUN apt-get install -y libsqlite3-dev",
                    "RUN make -C src ../aimee-kb ../aimee-kb-worm",
                    "FROM debian",
                    "RUN apt-get install -y \\",
                    "        postgresql-${PG_MAJOR}-pgvector \\",
                    "        libsqlite3-0 \\",
                    "        python3",
                    "ENV AIMEE_HOME=/var/lib/aimee",
                    "COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb",
                    "COPY --from=build /src/aimee-kb-worm /usr/local/bin/aimee-kb-worm",
                    "COPY scripts/embed-remote.py /opt/aimee/scripts/",
                    "COPY deploy/container/aimee.yaml /var/lib/aimee/.config/aimee/aimee.yaml",
                    "USER aimee",
                    "HEALTHCHECK CMD curl -fsS http://127.0.0.1:8741/v1/health || exit 1",
                    "EXPOSE 8741",
                    'ENTRYPOINT ["aimee-kb"]',
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (root / "compose.yaml").write_text(
            "\n".join(
                [
                    "services:",
                    "  aimee-kb:",
                    "    build:",
                    "      context: .",
                    "      dockerfile: Dockerfile",
                    "    environment:",
                    "      AIMEE_HOME: /var/lib/aimee",
                    "      SYNTHESIS_ENDPOINT: ${SYNTHESIS_ENDPOINT:-}",
                    "      AIMEE_KB_MTLS_HOST: aimee-kb",
                    '      AIMEE_KB_MTLS_PORT: "8745"',
                    "    ports:",
                    '      - "127.0.0.1:8741:8741"',
                    "    healthcheck:",
                    '      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8741/v1/health"]',
                    "  aimee-control-web:",
                    "    environment:",
                    "      AIMEE_CONTROL_WEB_ENABLED: ${AIMEE_CONTROL_WEB_ENABLED:-1}",
                    "      CONTROL_WEB_CRED_FILE: /run/control-web/console.cred",
                    "    volumes:",
                    "      - ${CONTROL_WEB_CRED_DIR:-./control-web-secrets}:/run/control-web:ro",
                    "    healthcheck:",
                    "      test:",
                    "        - CMD-SHELL",
                    "        - >-",
                    "          enabled=$$(printf '%s' \"$${AIMEE_CONTROL_WEB_ENABLED:-1}\" | tr '[:upper:]' '[:lower:]');",
                    "          case \"$$enabled\" in 0|false|no|off) exit 0;; esac;",
                    "          if [ ! -f \"$${CONTROL_WEB_CRED_FILE}\" ]; then exit 0; fi;",
                    "          curl -fsSk https://127.0.0.1:8744/",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        server_service = "\n".join(
            [
                "  aimee-server:",
                "    environment:",
                "      AIMEE_SERVER_ID: ${AIMEE_SERVER_ID:-}",
                "      AIMEE_SERVER_TEAM_ID: ${AIMEE_SERVER_TEAM_ID:-}",
                "      AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE: "
                "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-/run/aimee/management/jwks-trust-bundle.json}",
                "    volumes:",
                "      - ${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro",
                "",
            ]
        )
        managed_server_service = server_service.replace(
            "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-/run/aimee/management/jwks-trust-bundle.json}",
            "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-/run/aimee/managed-trust/jwks-trust-bundle.json}",
        ).replace(
            "      - ${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro",
            "      - ${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro\n"
            "      - aimee-managed-jwks-trust:/run/aimee/managed-trust:ro",
        )
        (root / "compose.server.yaml").write_text(
            read(root / "compose.yaml") + server_service, encoding="utf-8"
        )
        (root / "compose.server-managed.yaml").write_text(
            "services:\n" + managed_server_service, encoding="utf-8"
        )
        (root / "compose.server-standalone.yaml").write_text(
            "services:\n" + server_service, encoding="utf-8"
        )
        (root / "Dockerfile.server").write_text(
            "FROM debian\nENV AIMEE_HOME=/var/lib/aimee\n", encoding="utf-8"
        )
        (root / "deploy/container").mkdir(parents=True)
        (root / "deploy/container/aimee-managed.compose.yaml").write_text(
            "services:\n"
            "  aimee-kb:\n"
            "    environment:\n"
            "      AIMEE_KB_MTLS_HOST: aimee-kb\n"
            '      AIMEE_KB_MTLS_PORT: "8745"\n'
            "      SYNTHESIS_ENDPOINT: ${SYNTHESIS_ENDPOINT:-}\n"
            "      SYNTHESIS_AUTH_REQUIRED: ${SYNTHESIS_AUTH_REQUIRED:-0}\n"
            "      SYNTHESIS_MODEL: ${SYNTHESIS_MODEL:-aimee-synth}\n"
            "      EMBEDDER_DIMS: ${EMBEDDER_DIMS:-}\n",
            encoding="utf-8",
        )
        (root / "deploy/container/aimee-server-remote-writes.yaml").write_text(
            'aimee:\n  api:\n    remote_writes: "off"\n', encoding="utf-8"
        )
        (root / "src/server").mkdir(parents=True)
        (root / "src/server/server_tls.c").write_text(
            'config_default_dir(); "%s/tls/server.crt"; pki_ensure_self_signed_server_cert();\n',
            encoding="utf-8",
        )
        (root / ".dockerignore").write_text(
            "\n".join(sorted(REQUIRED_DOCKERIGNORE_ENTRIES)) + "\n",
            encoding="utf-8",
        )
        (root / ".env").write_text("COMPOSE_PROFILES=\n", encoding="utf-8")
        found = check(root)
        if found:
            print("kb-container-packaging plant: failed", file=sys.stderr)
            for item in found:
                print(f"  found: {item}", file=sys.stderr)
            return 1

        control_compose = read(root / "compose.yaml")
        for marker in (
            "      AIMEE_CONTROL_WEB_ENABLED: ${AIMEE_CONTROL_WEB_ENABLED:-1}\n",
            "      - ${CONTROL_WEB_CRED_DIR:-./control-web-secrets}:/run/control-web:ro\n",
            '          if [ ! -f \"$${CONTROL_WEB_CRED_FILE}\" ]; then exit 0; fi;\n',
            "          curl -fsSk https://127.0.0.1:8744/\n",
        ):
            planted = control_compose.replace(marker, "", 1)
            if planted == control_compose or not control_web_idle_health_failures(planted):
                print(
                    f"kb-container-packaging plant: missed control-web health marker {marker!r}",
                    file=sys.stderr,
                )
                return 1

        identity_compose = read(root / "compose.server-managed.yaml")
        for marker in (
            "      AIMEE_SERVER_ID: ${AIMEE_SERVER_ID:-}\n",
            "      AIMEE_SERVER_TEAM_ID: ${AIMEE_SERVER_TEAM_ID:-}\n",
            "      AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE: "
            "${AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE:-/run/aimee/managed-trust/jwks-trust-bundle.json}\n",
            "      - ${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:"
            "/run/aimee/management:ro\n",
            "      - aimee-managed-jwks-trust:/run/aimee/managed-trust:ro\n",
        ):
            planted = identity_compose.replace(marker, "", 1)
            if planted == identity_compose or not server_identity_failures(planted, managed=True):
                print(f"kb-container-packaging plant: missed server identity marker {marker!r}", file=sys.stderr)
                return 1

        for marker in (
            "      AIMEE_KB_MTLS_HOST: aimee-kb\n",
            '      AIMEE_KB_MTLS_PORT: "8745"\n',
        ):
            planted = read(root / "deploy/container/aimee-managed.compose.yaml").replace(
                marker, "", 1
            )
            if not kb_mtls_failures(planted):
                print(
                    f"kb-container-packaging plant: missed KB mTLS marker {marker!r}",
                    file=sys.stderr,
                )
                return 1

        # The sidecar may exist, but not ungated and not without the kb ordering it
        # depends on. Planting a bare service definition exercises both.
        managed_text = read(root / "deploy/container/aimee-managed.compose.yaml")
        planted_service = managed_text.replace(
            "  aimee-llm:\n    profiles: [\"llm\"]\n",
            "  aimee-llm-ungated:\n",
        ) + ("  aimee-llm:\n    image: ghcr.io/example/aimee-llm:latest\n")
        planted_failures = managed_kb_llm_contract_failures(planted_service)
        if not any("gated behind the 'llm' profile" in f for f in planted_failures):
            print(
                "kb-container-packaging plant: missed an ungated aimee-llm sidecar",
                file=sys.stderr,
            )
            return 1
        if not any("service_healthy" in f for f in planted_failures):
            print(
                "kb-container-packaging plant: missed an aimee-llm with no kb ordering",
                file=sys.stderr,
            )
            return 1

        planted_dependency = managed_text.replace(
            "    environment:\n",
            "    depends_on:\n"
            "      aimee-llm: { condition: service_healthy, required: false }\n"
            "    environment:\n",
            1,
        )
        if "managed KB must not depend on the retired aimee-llm service" not in (
            managed_kb_llm_contract_failures(planted_dependency)
        ):
            print(
                "kb-container-packaging plant: missed KB-to-LLM startup dependency",
                file=sys.stderr,
            )
            return 1

        if not server_default_config_failures(
            "aimee:\n  api:\n    remote_writes: data\n"
        ):
            print("kb-container-packaging plant: missed active legacy remote_writes default", file=sys.stderr)
            return 1

        (root / ".env").write_text("COMPOSE_PROFILES=some-profile\n", encoding="utf-8")
        if ".env enables optional Compose profiles by default" not in check(root):
            print("kb-container-packaging plant: missed enabled default profile", file=sys.stderr)
            return 1
        (root / ".env").write_text("COMPOSE_PROFILES=\n", encoding="utf-8")

        compose_text = read(root / "compose.yaml")
        (root / "compose.yaml").write_text(
            compose_text.replace("127.0.0.1:8741:8741", "8741:8741"), encoding="utf-8"
        )
        found = check(root)
        expected_port_failures = {
            "compose.yaml contains forbidden kb-port-non-loopback",
        }
        if not expected_port_failures.issubset(set(found)):
            print("kb-container-packaging plant: failed", file=sys.stderr)
            for item in found:
                print(f"  found: {item}", file=sys.stderr)
            return 1
        (root / "compose.yaml").write_text(compose_text, encoding="utf-8")

        bad_publications = [
            '      - "0.0.0.0:8741:8741"',
            '      - "127.0.0.1:8741:8741"\n      - "0.0.0.0:9999:22"',
            '      - "127.0.0.1:9999:8741"',
            '      - "127.0.0.1:08741:8741"',
            '      - "127.0.0.1:8741:8741/udp"',
            '      - "[::1]:8741:8741/udp"',
            "    network_mode: host",
            "    network_mode: host # trailing comment",
            "    network_mode: $KB_NETWORK_MODE",
            "      - target: 8741\n        published: 8741",
            "      - target: 22\n        published: 9999\n        host_ip: 0.0.0.0",
            "      - target: '08741'\n        published: 8741\n        host_ip: 127.0.0.1",
            "      - target: 8741\n        published: 8741-8750\n        host_ip: 127.0.0.1",
            "      - target: 8741\n        published: 8741\n        host_ip: 127.0.0.1\n        protocol: udp",
            "      - target: 8741\n        published: 8741\n        host_ip: '[::1]'\n        protocol: udp",
            "      - target: 8741\n        published: 8741\n        host_ip: 0.0.0.0",
            "      - target: 8741\n        published: 8741\n        host_ip: \"[::]\"",
            "      - target: 8741\n        published: 8741\n        host_ip: '*'",
            "    ports: [8741:8741]",
            '    ports: ["0.0.0.0:8741:8741"]',
            "    ports: [{target: 8741, published: 8741}]",
            "    ports: &kb_ports [8741:8741]",
            '      - "127.0.0.1:8741:8741"\n      - ${KB_PORT}:8741',
            '      - "127.0.0.1:8741:8741"\n      - $KB_PORT:8741',
        ]
        safe_compose = read(root / "compose.server.yaml")
        for bad in bad_publications:
            planted = safe_compose.replace('      - "127.0.0.1:8741:8741"', bad)
            if planted == safe_compose:
                print(f"kb-container-packaging plant: mutation did not apply for {bad!r}", file=sys.stderr)
                return 1
            if not kb_publication_failures(planted):
                print(f"kb-container-packaging plant: missed {bad!r}", file=sys.stderr)
                return 1

        merged_host_network = safe_compose.replace(
            "services:\n", "x-kb-network: &kb_network\n  network_mode: host\nservices:\n", 1
        ).replace("  aimee-kb:\n", "  aimee-kb:\n    <<: *kb_network\n", 1)
        if not kb_publication_failures(merged_host_network):
            print("kb-container-packaging plant: missed merged host networking", file=sys.stderr)
            return 1

        for static_mode in ("none", "service:postgres", "container:deadbeef"):
            planted = safe_compose.replace(
                "  aimee-kb:\n", f"  aimee-kb:\n    network_mode: {static_mode}\n", 1
            )
            if not kb_publication_failures(planted):
                print(f"kb-container-packaging plant: missed network_mode {static_mode}", file=sys.stderr)
                return 1

        # A block key left holding None after its last entry was deleted: valid
        # YAML, rejected by Compose at startup ("depends_on must be a array").
        planted_empty = safe_compose.replace(
            "    healthcheck:\n", "    depends_on:\n    healthcheck:\n", 1
        )
        if not any(
            "depends_on is present but empty" in failure
            for failure in empty_key_failures(planted_empty)
        ):
            print(
                "kb-container-packaging plant: missed present-but-empty service key",
                file=sys.stderr,
            )
            return 1

        # Any non-empty default is rejected now, including the retired container's own
        # host:port — there is nothing listening there to point a deploy at.
        for bad_llm in (
            "${SYNTHESIS_ENDPOINT:-http://aimee-llm.attacker.example}",
            "${SYNTHESIS_ENDPOINT:-http://aimee-llm:8080}",
        ):
            planted = safe_compose.replace("${SYNTHESIS_ENDPOINT:-}", bad_llm, 1)
            if not kb_publication_failures(planted):
                print(f"kb-container-packaging plant: missed LLM URL {bad_llm}", file=sys.stderr)
                return 1

        for bad_dockerfile in ("Dockerfile.attacker", "${KB_DOCKERFILE:-Dockerfile}"):
            planted = safe_compose.replace(
                "  aimee-kb:\n    build:\n      context: .\n      dockerfile: Dockerfile",
                "  aimee-kb:\n    build:\n      context: .\n"
                f"      dockerfile: {bad_dockerfile}",
                1,
            )
            if not kb_publication_failures(planted):
                print(
                    f"kb-container-packaging plant: missed build.dockerfile {bad_dockerfile}",
                    file=sys.stderr,
                )
                return 1

        for bad_build in (".", "./attacker"):
            planted = safe_compose.replace(
                "  aimee-kb:\n    build:\n      context: .\n      dockerfile: Dockerfile",
                f"  aimee-kb:\n    build: {bad_build}",
                1,
            )
            if not kb_publication_failures(planted):
                print(f"kb-container-packaging plant: missed scalar build {bad_build}", file=sys.stderr)
                return 1

        extra_port_entries = (
            '      - "127.0.0.1:8741:8741"\n      - "127.0.0.1:8741:8741"',
            '      - "127.0.0.1:8741:8741"\n'
            "      - target: 8741\n        published: 8741\n        protocol: tcp",
        )
        for entries in extra_port_entries:
            planted = safe_compose.replace('      - "127.0.0.1:8741:8741"', entries, 1)
            if not kb_publication_failures(planted):
                print("kb-container-packaging plant: missed extra KB port entry", file=sys.stderr)
                return 1

        duplicate_service_key = safe_compose.replace(
            "      AIMEE_HOME: /var/lib/aimee\n",
            "      AIMEE_HOME: /var/lib/aimee\n      AIMEE_HOME: /tmp/attacker\n",
            1,
        )
        if not kb_publication_failures(duplicate_service_key):
            print("kb-container-packaging plant: missed duplicate YAML mapping key", file=sys.stderr)
            return 1

        ranged_publication = safe_compose.replace(
            '      - "127.0.0.1:8741:8741"',
            '      - "127.0.0.1:8741:8741"\n      - "9700-9800:8700-8800"',
            1,
        )
        if not kb_publication_failures(ranged_publication):
            print("kb-container-packaging plant: missed short-form port range", file=sys.stderr)
            return 1

        ipv6_loopback = safe_compose.replace(
            '      - "127.0.0.1:8741:8741"', '      - "[::1]:8741:8741"'
        )
        if kb_publication_failures(ipv6_loopback):
            print("kb-container-packaging plant: rejected IPv6 loopback", file=sys.stderr)
            return 1

        ipv6_long_loopback = safe_compose.replace(
            '      - "127.0.0.1:8741:8741"',
            "      - target: 8741\n        published: 8741\n        host_ip: '[::1]'\n        protocol: tcp",
        )
        if kb_publication_failures(ipv6_long_loopback):
            print("kb-container-packaging plant: rejected long-form IPv6 loopback", file=sys.stderr)
            return 1

        if not has_compose_interpolation("$KB_PORT") or not has_compose_interpolation("${KB_PORT}"):
            print("kb-container-packaging plant: missed Compose interpolation", file=sys.stderr)
            return 1
        if has_compose_interpolation("$$KB_PORT"):
            print("kb-container-packaging plant: rejected Compose literal-dollar escape", file=sys.stderr)
            return 1

        for hidden_src in ("./src", ".//src", "/src/"):
            (root / ".dockerignore").write_text(
                "\n".join(sorted(REQUIRED_DOCKERIGNORE_ENTRIES | {hidden_src})) + "\n",
                encoding="utf-8",
            )
            if ".dockerignore hides required build input src" not in check(root):
                print(f"kb-container-packaging plant: missed normalized {hidden_src}", file=sys.stderr)
                return 1

        (root / ".dockerignore").write_text(
            "\n".join(sorted(REQUIRED_DOCKERIGNORE_ENTRIES | {"src"})) + "\n",
            encoding="utf-8",
        )
        found = check(root)
        expected_context_failure = ".dockerignore hides required build input src"
        if expected_context_failure in found:
            print("kb-container-packaging plant: ok")
            return 0
        print("kb-container-packaging plant: failed", file=sys.stderr)
        for item in found:
            print(f"  found: {item}", file=sys.stderr)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Check aimee-kb container packaging.")
    parser.add_argument("--root", default=".", help="Repository root")
    parser.add_argument("--plant-test", action="store_true", help="Run planted failure self-test")
    args = parser.parse_args()

    if args.plant_test:
        return plant_test()

    failures = check(Path(args.root).resolve())
    if failures:
        print("kb-container-packaging: violations found:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("kb-container-packaging: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
