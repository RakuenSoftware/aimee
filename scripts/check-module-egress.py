#!/usr/bin/env python3
"""Refuse an unaudited outbound network call from a Go process module."""

from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "server-go" / "modules"

# These packages own network boundaries rather than consume ordinary outbound
# HTTP. Observability is a core component, metrics_server is inbound, and the
# sandbox/delegate proxies are the already isolated sole-egress implementation.
EXEMPT = {
    "egress/egress.go",
    "observability/observability.go",
    "observability/metrics_server.go",
    "sandbox/proxy.go",
    "delegates/cmd/aimee-delegate-egress/main.go",
}

DIRECT = re.compile(r"\b(?:http\.(?:Get|Post|PostForm)|net\.Dial|DialContext)\s*\(|\.Do\s*\(")
HTTP_IMPORT = re.compile(r'"net/http"')
TRANSPORT = re.compile(r"egress\.(?:HTTPRequest|SSERequest)|\.OpenSSE\s*\(")


def violation(relative: str, source: str) -> str | None:
    source = re.sub(r"\b\w*[Oo]nce\.Do\s*\(", "", source)
    direct = DIRECT.search(source) and (HTTP_IMPORT.search(source) or "net.Dial" in source or "DialContext" in source)
    if relative.startswith("egress/") or relative in EXEMPT or not direct:
        return None
    return f"{relative}: direct network transport is forbidden outside a declared network owner"


def runtime_guard_failures() -> list[str]:
    """Keep source review and the runtime deny-by-default boundary coupled."""
    main = (ROOT / "server-go/cmd/aimee-module/main.go").read_text(encoding="utf-8")
    guard = (ROOT / "server-go/cmd/aimee-module/network_guard_linux.go").read_text(encoding="utf-8")
    failures = []
    required_main = (
        'config.ModuleName != "egress"',
        'config.ModuleName != "postgres"',
        'config.ModuleName != "sandbox"',
        "installModuleNetworkGuard()",
        "config.AfterAttach = hardenEgressCredentialOwner",
    )
    missing_main = [item for item in required_main if item not in main]
    if missing_main:
        failures.append("module launcher network guard contract is incomplete: " + ", ".join(missing_main))
    required_guard = ("unix.AF_INET", "unix.AF_INET6", "unix.SYS_SOCKET", "unix.SECCOMP_FILTER_FLAG_TSYNC")
    missing_guard = [item for item in required_guard if item not in guard]
    if missing_guard:
        failures.append("Linux socket filter contract is incomplete: " + ", ".join(missing_guard))
    credential = (ROOT / "server-go/modules/egress/credential.go").read_text(encoding="utf-8")
    resolver = (ROOT / "server-go/modules/egress/secret_resolver.go").read_text(encoding="utf-8")
    transports = resolver + credential + (ROOT / "server-go/modules/egress/http.go").read_text(encoding="utf-8") + (
        ROOT / "server-go/modules/egress/sse.go"
    ).read_text(encoding="utf-8")
    forge = (ROOT / "server-go/modules/git/forge_request.go").read_text(encoding="utf-8")
    for path, forbidden in (
        ("egress SSE", 'json:"credential_env'),
        ("git forge", 'json:"token"'),
        ("git forge", '"Authorization": "Bearer "'),
    ):
        source = transports if path == "egress SSE" else forge
        if forbidden in source:
            failures.append(f"{path} credential custody regressed: found {forbidden}")
    for required in ("CredentialEnvelope", "credentialMaxLifetime", "CredentialHandle"):
        if required not in transports:
            failures.append(f"egress credential custody contract is incomplete: {required}")

    # The transport-side types are only half the custody boundary. Keep the
    # installed helper, its parent attestation, and the C envelope client under
    # the same ratchet so a packaging edit cannot silently put plaintext back
    # into an ordinary module environment or process dump.
    required_by_path = {
        "src/egress_credential_envelope.c": (
            "EVP_PKEY_X25519",
            "EVP_aes_256_gcm",
            "CREDENTIAL_LIFETIME_SECS 30",
            "AIMEE_EGRESS_EVENT_CREDENTIAL_KEY",
        ),
        "src/modules/vault/vault_env_bootstrap.c": (
            '"/usr/local/libexec/aimee-modules/aimee-module-egress"',
            "PR_SET_DUMPABLE",
            "vault_env_print_egress_credential",
        ),
        "src/server/server_main.c": ('"--egress-vault-secret"',),
        "src/kb/kb_main.c": ('"--egress-vault-secret"',),
        "deploy/container/server-entrypoint.sh": (
            "AIMEE_EGRESS_CREDENTIAL_HELPER=/usr/local/bin/aimee-server",
        ),
        "deploy/container/aimee-kb-entrypoint.sh": (
            "AIMEE_EGRESS_CREDENTIAL_HELPER=/usr/local/bin/aimee-kb",
        ),
    }
    for relative, required_tokens in required_by_path.items():
        source = (ROOT / relative).read_text(encoding="utf-8")
        for required in required_tokens:
            if required not in source:
                failures.append(f"egress credential custody contract is incomplete: {relative}: {required}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plant-test", action="store_true")
    args = parser.parse_args()
    if args.plant_test:
        planted = 'package planted\nimport "net/http"\nfunc x(c *http.Client, r *http.Request) { c.Do(r) }\n'
        if violation("planted/bypass.go", planted) is None:
            print("check-module-egress: planted bypass was not detected", file=sys.stderr)
            return 1
        print("check-module-egress: plant test passed")
        return 0

    failures = runtime_guard_failures()
    governed = set()
    for path in sorted(MODULES.rglob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        relative = path.relative_to(MODULES).as_posix()
        source = path.read_text(encoding="utf-8")
        problem = violation(relative, source)
        if problem:
            failures.append(problem)
        elif TRANSPORT.search(source) and relative not in EXEMPT:
            governed.add(relative)

    expected = {
        "git/forge_request.go",
        "mcp/sse.go",
        "memory/embed.go",
        "roundtable/panel/artifact.go",
    }
    missing = expected - governed
    if missing:
        failures.append("governed outbound inventory disappeared: " + ", ".join(sorted(missing)))
    if failures:
        print("check-module-egress: FAIL", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print(f"check-module-egress: ok ({len(governed)} bus-transport clients; direct sockets confined to network owners)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
