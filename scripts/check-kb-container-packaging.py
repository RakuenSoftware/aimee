#!/usr/bin/env python3
"""Check static packaging invariants for the aimee-kb container."""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path


REQUIRED_DOCKERFILE_PATTERNS = {
    "named-build-stage": r"(?im)^FROM\s+\S+\s+AS\s+build\b",
    "builds-aimee-kb": r"make\s+-C\s+src\s+\.\./aimee-kb",
    "copies-aimee-kb": r"COPY\s+--from=build\s+/src/aimee-kb\s+/usr/local/bin/aimee-kb",
    "aimee-home-env": r"(?m)^ENV\s+AIMEE_HOME=/var/lib/aimee\b",
    "db2-url-env": r"AIMEE_DB2_URL=.*postgres",
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
}

REQUIRED_COMPOSE_PATTERNS = {
    "postgres-service": r"(?m)^\s{2}postgres:",
    "kb-service": r"(?m)^\s{2}aimee-kb:",
    "kb-build-context": r"(?s)aimee-kb:.*build:.*context:\s*\.",
    "aimee-home-env": r"(?s)aimee-kb:.*AIMEE_HOME:\s*/var/lib/aimee",
    "db2-url-env": r"AIMEE_DB2_URL[=:].*postgres",
    "kb-port": r'"?8741:8741"?',
    "postgres-health": r"condition:\s+service_healthy",
    "kb-health": r"(?s)aimee-kb:.*healthcheck:.*http://127\.0\.0\.1:8741/v1/health",
    # The unified aimee-llm container backs real embeddings/reranking/synthesis;
    # the kb is pointed at it by AIMEE_LLM_URL (no model runs in the kb). The
    # legacy torch embedder is retained behind a profile for rollback.
    "llm-service": r"(?m)^\s{2}aimee-llm:",
    "embedder-service": r"(?m)^\s{2}embedder:",
    "llm-url-env": r"AIMEE_LLM_URL:\s*\$\{AIMEE_LLM_URL:-http://aimee-llm",
}

REQUIRED_DOCKERIGNORE_ENTRIES = {
    ".git",
    ".aimee",
    "build",
    "src/build",
    "frontend/node_modules",
    "/aimee",
    "/aimee-server",
    "/aimee-kb",
    "/aimee-webchat",
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
        entries.add(line.rstrip("/"))
    return entries


def missing_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if not re.search(pattern, text)]


def present_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if re.search(pattern, text)]


def check(root: Path) -> list[str]:
    failures: list[str] = []
    dockerfile = root / "Dockerfile"
    compose = root / "compose.yaml"
    dockerignore = root / ".dockerignore"

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

    if not dockerignore.exists():
        failures.append("missing .dockerignore")
    else:
        entries = normalized_dockerignore(dockerignore)
        for entry in sorted(REQUIRED_DOCKERIGNORE_ENTRIES - entries):
            failures.append(f".dockerignore missing {entry}")
        for entry in sorted(FORBIDDEN_DOCKERIGNORE_ENTRIES & entries):
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
            "Dockerfile missing db2-url-env",
            "Dockerfile missing runtime-user",
            "Dockerfile missing health-v1",
            "Dockerfile missing exposes-http",
            "Dockerfile missing entrypoint-kb",
            "Dockerfile missing python3-runtime",
            "Dockerfile missing copies-sidecars",
            "Dockerfile missing copies-config",
            "Dockerfile contains forbidden server-binary",
            "compose.yaml missing postgres-service",
            "compose.yaml missing kb-build-context",
            "compose.yaml missing aimee-home-env",
            "compose.yaml missing db2-url-env",
            "compose.yaml missing kb-port",
            "compose.yaml missing postgres-health",
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
                    "ENV AIMEE_DB2_URL=postgresql://aimee:aimee@postgres:5432/aimee_shared",
                    "RUN make -C src ../aimee-kb",
                    "FROM debian",
                    "RUN apt-get install -y \\",
                    "        python3",
                    "ENV AIMEE_HOME=/var/lib/aimee",
                    "ENV AIMEE_DB2_URL=postgresql://aimee:aimee@postgres:5432/aimee_shared",
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
                    "  postgres:",
                    "    image: pgvector/pgvector:pg16",
                    "    healthcheck:",
                    '      test: ["CMD-SHELL", "pg_isready -U aimee"]',
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
                    "      AIMEE_DB2_URL: postgresql://aimee:aimee@postgres:5432/aimee_shared",
                    "      AIMEE_LLM_URL: ${AIMEE_LLM_URL:-http://aimee-llm:8080}",
                    "    ports:",
                    '      - "8741:8741"',
                    "    depends_on:",
                    "      postgres:",
                    "        condition: service_healthy",
                    "    healthcheck:",
                    '      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8741/v1/health"]',
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (root / ".dockerignore").write_text(
            "\n".join(sorted(REQUIRED_DOCKERIGNORE_ENTRIES)) + "\n",
            encoding="utf-8",
        )
        found = check(root)
        if found:
            print("kb-container-packaging plant: failed", file=sys.stderr)
            for item in found:
                print(f"  found: {item}", file=sys.stderr)
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
