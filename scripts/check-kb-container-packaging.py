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
    "copies-aimee-kb": r"COPY\s+--from=build\s+/src/aimee-kb\s+/usr/local/bin/aimee-kb",
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
    "sqlite-package": r"sqlite3|libsqlite3",
    "db1-reference": r"\bDB1\b|db1/",
    # A baked URL makes an unconfigured install silently depend on a sibling
    # container instead of starting the PostgreSQL shipped inside aimee-kb.
    "db2-url-pinned": r"(?m)^ENV\s+AIMEE_DB2_URL=",
}

REQUIRED_COMPOSE_PATTERNS = {
    "kb-service": r"(?m)^\s{2}aimee-kb:",
    "kb-build-context": r"(?s)aimee-kb:.*build:.*context:\s*\.",
    "aimee-home-env": r"(?s)aimee-kb:.*AIMEE_HOME:\s*/var/lib/aimee",
    # Empty is the new-install contract: the image entrypoint starts its bundled
    # PostgreSQL. The interpolation still lets an existing deployment opt into an
    # external DB2 through its environment without editing the manifest.
    "db2-empty-default": r"(?m)^\s*AIMEE_DB2_URL:\s*\$\{AIMEE_DB2_URL:-\}\s*$",
    "kb-health": r"(?s)aimee-kb:.*healthcheck:.*http://127\.0\.0\.1:8741/v1/health",
    # The unified aimee-llm container backs real embeddings/reranking/synthesis;
    # the kb is pointed at it by AIMEE_LLM_URL (no model runs in the kb). The
    # legacy torch embedder is retained behind a profile for rollback.
    "llm-service": r"(?m)^\s{2}aimee-llm:",
    "embedder-service": r"(?m)^\s{2}embedder:",
    "llm-url-env": r"(?m)^\s*AIMEE_LLM_URL:\s*\$\{AIMEE_LLM_URL:-http://aimee-llm:8080\}\s*$",
}

FORBIDDEN_COMPOSE_PATTERNS = {
    "kb-port-non-loopback": r'(?m)^\s*-\s*["\']?8741:8741["\']?\s*$',
    "standalone-postgres-service": r"(?m)^\s{2}postgres:",
}

REQUIRED_DOCKERIGNORE_ENTRIES = {
    ".git",
    ".aimee",
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
    required_services = {"aimee-kb", "aimee-llm", "embedder"}
    missing_services = required_services - set(services)
    if missing_services:
        failures.append("missing required services: " + ", ".join(sorted(missing_services)))
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
        if environment.get("AIMEE_DB2_URL") != "${AIMEE_DB2_URL:-}":
            failures.append("aimee-kb AIMEE_DB2_URL must have an exact empty new-install default")
        if environment.get("AIMEE_LLM_URL") != "${AIMEE_LLM_URL:-http://aimee-llm:8080}":
            failures.append("aimee-kb AIMEE_LLM_URL must use the exact aimee-llm:8080 default")
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


def check(root: Path) -> list[str]:
    failures: list[str] = []
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
    server_tls = root / "src" / "server" / "server_tls.c"
    if not server_dockerfile.exists() or "ENV AIMEE_HOME=/var/lib/aimee" not in read(
        server_dockerfile
    ):
        failures.append("server image missing AIMEE_HOME=/var/lib/aimee certificate root")
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

    if not server_compose.exists():
        failures.append("missing compose.server.yaml")
    else:
        for failure in kb_publication_failures(read(server_compose)):
            failures.append(f"compose.server.yaml {failure}")

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
            "compose.yaml missing db2-empty-default",
            "compose.yaml missing kb-health",
            "compose.yaml missing llm-service",
            "compose.yaml missing embedder-service",
            "compose.yaml missing llm-url-env",
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
                    "RUN make -C src ../aimee-kb",
                    "FROM debian",
                    "RUN apt-get install -y \\",
                    "        postgresql-${PG_MAJOR}-pgvector \\",
                    "        python3",
                    "ENV AIMEE_HOME=/var/lib/aimee",
                    "COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb",
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
                    "  embedder:",
                    "    build:",
                    "      context: .",
                    "      dockerfile: Dockerfile.embedder",
                    "    profiles: [\"legacy-embedder\"]",
                    "  aimee-llm:",
                    "    build:",
                    "      context: .",
                    "      dockerfile: Dockerfile.aimee-llm",
                    "  aimee-kb:",
                    "    build:",
                    "      context: .",
                    "      dockerfile: Dockerfile",
                    "    environment:",
                    "      AIMEE_HOME: /var/lib/aimee",
                    "      AIMEE_DB2_URL: ${AIMEE_DB2_URL:-}",
                    "      AIMEE_LLM_URL: ${AIMEE_LLM_URL:-http://aimee-llm:8080}",
                    "    ports:",
                    '      - "127.0.0.1:8741:8741"',
                    "    healthcheck:",
                    '      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8741/v1/health"]',
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (root / "compose.server.yaml").write_text(
            read(root / "compose.yaml") + "  aimee-server:\n", encoding="utf-8"
        )
        (root / "compose.server-managed.yaml").write_text(
            "services:\n  aimee-server:\n", encoding="utf-8"
        )
        (root / "compose.server-standalone.yaml").write_text(
            "services:\n  aimee-server:\n", encoding="utf-8"
        )
        (root / "Dockerfile.server").write_text(
            "FROM debian\nENV AIMEE_HOME=/var/lib/aimee\n", encoding="utf-8"
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

        (root / ".env").write_text("COMPOSE_PROFILES=curator-llm\n", encoding="utf-8")
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

        for bad_llm in (
            "${AIMEE_LLM_URL:-http://aimee-llm.attacker.example}",
            "${AIMEE_LLM_URL:-http://aimee-llm:8080/path}",
        ):
            planted = safe_compose.replace(
                "${AIMEE_LLM_URL:-http://aimee-llm:8080}", bad_llm, 1
            )
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
