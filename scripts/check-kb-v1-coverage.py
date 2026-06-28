#!/usr/bin/env python3
"""Track aimee-server -> aimee-kb RPC migration coverage.

Client calls must use /v1 endpoints. The legacy Unix-socket transport has been
fully retired: kb_client*.c no longer opens the socket or sends any RPC method
(including the former server.info readiness probe and the v1.http tunnel), so
the legacy baselines are empty and any reintroduced socket method fails the gate.
"""

import argparse
import pathlib
import re
import sys
import tempfile
from typing import Iterable, Set


METHOD_RE = re.compile(r'"(kb\.[A-Za-z0-9_.-]+)"')
LEGACY_DIRECTIVE_RES = (
    re.compile(
        r'(?:kb_directive_request|kb_learning_request|kb_learning_mutate|'
        r'kb_memory_embed_request|kb_client_simple_count_request|'
        r'kb_client_collab_rules_action|kb_client_session_briefing_section|'
        r'kb_client_dashboard_payload_request)\(\s*"([^"]+)"'
    ),
    re.compile(r'cJSON_AddStringToObject\(\s*req\s*,\s*"method"\s*,\s*"([^"]+)"\s*\)'),
)
V1_ROUTE_RE = re.compile(r'"/v1/[^"]+"')
OPENAPI_PATH_RE = re.compile(r"^  (/[^:]+):\s*$")
OPENAPI_METHOD_RE = re.compile(r"^    (get|head|post|put|patch|delete):\s*$")
SERVICE_RPC_DISPATCH_RE = re.compile(
    r'strcmp\(\s*method->valuestring\s*,\s*"([^"]+)"\s*\)\s*==\s*0'
)


LEGACY_BASELINE = set()

LEGACY_DIRECTIVE_BASELINE = set()

V1_BRIDGE_METHODS = {
    "v1.http",
}

# Migrated methods are deliberately absent from LEGACY_BASELINE: if client code
# reintroduces them as kb.* RPCs, the scan reports them as unexpected.
MIGRATED_METHODS = {
    "kb.build",
    "kb.health",
    "kb.ingest",
    "kb.ingest.job.claim",
    "kb.ingest.job.complete",
    "kb.ingest.job.fail",
    "kb.ingest.status",
    "kb.search",
    "kb.status",
    "kb.update",
    "kb.workers",
    "kb.chunks.store",
    "kb.file_index.snapshot",
}

RETIRED_SERVICE_RPC_METHODS = MIGRATED_METHODS | {
    "kb.clear",
    "kb.queue_drain",
    "kb.queue_status",
    "kb.reconcile",
    "kb.repair",
    "index.blast_radius",
    "index.blast_radius_preview",
    "index.code_search",
    "index.find",
    "index.find_callers",
    "index.list",
    "index.project_lang",
    "index.project_stats",
    "index.scan",
    "index.structure",
}

RETIRED_DIRECTIVE_RPC_METHODS = {
    "bandit.export",
    "calibrate.check_readiness",
    "demote.check_readiness",
}


METHOD_ENDPOINTS = {
    "kb.health": ("GET /v1/health", "GET /v1/version"),
    "kb.status": ("GET /v1/health", "GET /v1/version"),
    "kb.search": ("POST /v1/search",),
    "kb.ingest": ("POST /v1/ingest",),
    "kb.build": ("POST /v1/code/build",),
    "kb.update": ("POST /v1/code/update",),
    "kb.ingest.status": ("GET /v1/ingest/status",),
    "kb.queue_status": ("GET /v1/pipeline/status", "GET /v1/jobs/{id}"),
    "kb.queue_drain": ("POST /v1/drain",),
    "kb.canonical_index.scan": ("POST /v1/code/scan",),
    "kb.repair": ("POST /v1/maintenance/repair",),
    "kb.reconcile": ("POST /v1/maintenance/reconcile",),
    "kb.clear": ("POST /v1/maintenance/clear",),
    "kb.workers": ("GET /v1/workers",),
}


OPENAPI_EXTRA_ENDPOINTS = {
    "POST /v1/actions/{action}",
    "POST /v1/docs",
    "POST /v1/docs/manifest",
    "GET /v1/docs/{id}",
    "DELETE /v1/docs/{id}",
    "GET /v1/code/callers",
    "GET /v1/code/project-stats",
    "GET /v1/code/cross-repo-deps",
    "POST /v1/code/repo-trust",
    "GET /v1/code/projects",
    "GET /v1/code/search",
    "GET /v1/intelligence/bandit/export",
    "GET /v1/intelligence/calibration/readiness",
    "GET /v1/intelligence/demotion/check",
}


INTERNAL_CLIENT_SYMBOLS = {
    "kb_client_v1_base_url",
    "kb_client_v1_post_json",
    "kb_client_v1_post_body",
    "kb_client_v1_post_body_with_type",
    "kb_client_v1_get_json",
    "kb_client_query_escape",
}

ALLOWED_INTERNAL_CLIENT_HEADER_USERS = {
    "server/kb_client.c",
    "server/kb_client_docs.c",
    "server/kb_client_index.c",
    "server/kb_client_pdf.c",
    "server/kb_client_code_graph.c",
    "server/kb_client_ws.c",
    "tests/test_kb_client_docs.c",
    "tests/test_kb_client_search.c",
}

ROOT_PATH_SCAN_RE = re.compile(r"\bkb_client_canonical_index_scan\s*\(")
ALLOWED_ROOT_PATH_SCAN_USERS = {
    "server/kb_client.c",
}

LEGACY_DIRECTIVE_CALL_RE = re.compile(
    r"\b(?:kb_directive_request|kb_learning_request|kb_learning_mutate|"
    r"kb_memory_embed_request|kb_client_simple_count_request|"
    r"kb_client_collab_rules_action|kb_client_session_briefing_section|"
    r"kb_client_dashboard_payload_request)\s*\("
)
ALLOWED_LEGACY_DIRECTIVE_CALL_USERS: set[str] = set()


def client_files(src_dir: pathlib.Path) -> Iterable[pathlib.Path]:
    files = set(src_dir.glob("kb_client*.c"))
    server_dir = src_dir / "server"
    if server_dir.is_dir():
        files.update(server_dir.rglob("kb_client*.c"))
    return sorted(files)


def http_files(src_dir: pathlib.Path) -> Iterable[pathlib.Path]:
    files = set(src_dir.glob("kb_http*.c"))
    kb_dir = src_dir / "kb"
    if kb_dir.is_dir():
        files.update(kb_dir.rglob("kb_http*.c"))
    return sorted(files)


def read_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="latin-1")


def source_files(src_dir: pathlib.Path) -> Iterable[pathlib.Path]:
    patterns = ("*.c", "*.h")
    files: set[pathlib.Path] = set()
    for pattern in patterns:
        files.update(src_dir.rglob(pattern))
    return sorted(files)


def scan_methods(paths: Iterable[pathlib.Path]) -> Set[str]:
    methods: Set[str] = set()
    for path in paths:
        methods.update(METHOD_RE.findall(read_text(path)))
    return methods


def scan_legacy_directive_methods(paths: Iterable[pathlib.Path]) -> Set[str]:
    methods: Set[str] = set()
    for path in paths:
        text = read_text(path)
        for pattern in LEGACY_DIRECTIVE_RES:
            methods.update(pattern.findall(text))
    return methods - V1_BRIDGE_METHODS


def scan_routes(paths: Iterable[pathlib.Path]) -> Set[str]:
    routes: Set[str] = set()
    for path in paths:
        for match in V1_ROUTE_RE.findall(read_text(path)):
            routes.add(match.strip('"'))
    return routes


def scan_openapi_endpoints(path: pathlib.Path) -> Set[str]:
    endpoints: Set[str] = set()
    current_path = None
    for line in read_text(path).splitlines():
        path_match = OPENAPI_PATH_RE.match(line)
        if path_match:
            current_path = path_match.group(1)
            continue
        method_match = OPENAPI_METHOD_RE.match(line)
        if current_path and method_match:
            endpoints.add(f"{method_match.group(1).upper()} /v1{current_path}")
    return endpoints


def normalize_endpoint(endpoint: str) -> str:
    return re.sub(r"\{[^}]+\}", "{}", endpoint)


def check_openapi(openapi_path: pathlib.Path) -> int:
    if not openapi_path.exists():
        print(f"kb-v1-coverage: OpenAPI spec not found: {openapi_path}", file=sys.stderr)
        return 1

    public_mapped = {
        endpoint
        for endpoints in METHOD_ENDPOINTS.values()
        for endpoint in endpoints
        if "/internal/" not in endpoint
    }
    required = {normalize_endpoint(endpoint) for endpoint in public_mapped | OPENAPI_EXTRA_ENDPOINTS}
    documented = {normalize_endpoint(endpoint) for endpoint in scan_openapi_endpoints(openapi_path)}
    internal = sorted(endpoint for endpoint in documented if " /v1/internal/" in endpoint)
    if internal:
        print("kb-v1-coverage: internal routes exposed in public OpenAPI:", file=sys.stderr)
        for endpoint in internal:
            print(f"  - {endpoint}", file=sys.stderr)
        return 1
    missing = sorted(required - documented)
    if missing:
        print("kb-v1-coverage: public /v1 routes missing from OpenAPI:", file=sys.stderr)
        for endpoint in missing:
            print(f"  - {endpoint}", file=sys.stderr)
        return 1
    return 0


def check_public_client_header(src_dir: pathlib.Path) -> int:
    header = src_dir / "headers" / "kb_client.h"
    if not header.exists():
        print(f"kb-v1-coverage: public client header not found: {header}", file=sys.stderr)
        return 1
    exposed = public_client_header_exposed_symbols(src_dir)
    if exposed:
        print("kb-v1-coverage: internal kb-client helpers exposed in kb_client.h:", file=sys.stderr)
        for symbol in exposed:
            print(f"  - {symbol}", file=sys.stderr)
        return 1
    return 0


def public_client_header_exposed_symbols(src_dir: pathlib.Path) -> list[str]:
    header = src_dir / "headers" / "kb_client.h"
    if not header.exists():
        return []
    text = read_text(header)
    return sorted(symbol for symbol in INTERNAL_CLIENT_SYMBOLS if symbol in text)


def scan_internal_client_header_includes(src_dir: pathlib.Path) -> list[str]:
    offenders: list[str] = []
    include_re = re.compile(r'^\s*#\s*include\s+"kb_client_internal\.h"', re.MULTILINE)
    for path in source_files(src_dir):
        rel = path.relative_to(src_dir).as_posix()
        if rel == "headers/kb_client_internal.h":
            continue
        if include_re.search(read_text(path)) and rel not in ALLOWED_INTERNAL_CLIENT_HEADER_USERS:
            offenders.append(rel)
    return sorted(offenders)


def check_internal_client_header_includes(src_dir: pathlib.Path) -> int:
    offenders = scan_internal_client_header_includes(src_dir)
    if offenders:
        print("kb-v1-coverage: kb_client_internal.h included outside approved bridge files:",
              file=sys.stderr)
        for offender in offenders:
            print(f"  - {offender}", file=sys.stderr)
        print("Move worker-only calls behind an approved bridge instead of widening the internal client surface.",
              file=sys.stderr)
        return 1
    return 0


def scan_root_path_scan_users(src_dir: pathlib.Path) -> list[str]:
    offenders: list[str] = []
    server_dir = src_dir / "server"
    if not server_dir.is_dir():
        return offenders
    for path in sorted(server_dir.rglob("*.c")):
        rel = path.relative_to(src_dir).as_posix()
        if rel in ALLOWED_ROOT_PATH_SCAN_USERS:
            continue
        if ROOT_PATH_SCAN_RE.search(read_text(path)):
            offenders.append(rel)
    return offenders


def scan_retired_service_rpc_dispatch(src_dir: pathlib.Path) -> list[str]:
    service = src_dir / "kb" / "kb_service.c"
    if not service.exists():
        return []
    found = {
        method
        for method in SERVICE_RPC_DISPATCH_RE.findall(read_text(service))
        if method in RETIRED_SERVICE_RPC_METHODS | RETIRED_DIRECTIVE_RPC_METHODS
    }
    return sorted(found)


def check_retired_service_rpc_dispatch(src_dir: pathlib.Path) -> int:
    offenders = scan_retired_service_rpc_dispatch(src_dir)
    if offenders:
        print("kb-v1-coverage: retired aimee-kb RPC methods still dispatched:", file=sys.stderr)
        for method in offenders:
            print(f"  - {method}", file=sys.stderr)
        print(
            "Route these operations through v1.http and the /v1 HTTP router only.",
            file=sys.stderr,
        )
        return 1
    return 0


def scan_rogue_directive_calls(src_dir: pathlib.Path) -> list[str]:
    offenders: list[str] = []
    kb_client_set = {p.relative_to(src_dir).as_posix() for p in client_files(src_dir)}
    for path in source_files(src_dir):
        rel = path.relative_to(src_dir).as_posix()
        if rel in kb_client_set or rel in ALLOWED_LEGACY_DIRECTIVE_CALL_USERS:
            continue
        if LEGACY_DIRECTIVE_CALL_RE.search(read_text(path)):
            offenders.append(rel)
    return sorted(offenders)


def check_rogue_directive_calls(src_dir: pathlib.Path) -> int:
    offenders = scan_rogue_directive_calls(src_dir)
    if offenders:
        print("kb-v1-coverage: legacy directive call functions used outside kb_client layer:",
              file=sys.stderr)
        for offender in offenders:
            print(f"  - {offender}", file=sys.stderr)
        print(
            "Use kb_v1_action_request (for action-style calls) or a typed kb_client_* wrapper.",
            file=sys.stderr,
        )
        return 1
    return 0


def check_root_path_scan_users(src_dir: pathlib.Path) -> int:
    offenders = scan_root_path_scan_users(src_dir)
    if offenders:
        print("kb-v1-coverage: server code calls root-path canonical scan:", file=sys.stderr)
        for offender in offenders:
            print(f"  - {offender}", file=sys.stderr)
        print(
            "Remote/headless kb ingest must push file bytes via "
            "kb_client_canonical_index_push_scan instead.",
            file=sys.stderr,
        )
        return 1
    return 0


def endpoint_route_available(endpoint: str, routes: Set[str]) -> bool:
    if endpoint == "kb-internal":
        return True
    parts = endpoint.split(maxsplit=1)
    if len(parts) != 2:
        return False
    route = parts[1]
    if "{id}" in route:
        prefix = route.split("{id}", 1)[0]
        return any(found.startswith(prefix) for found in routes)
    return route in routes


def plant_test() -> int:
    with tempfile.TemporaryDirectory(prefix="kb_v1_coverage_") as tmp:
        root = pathlib.Path(tmp)
        server_dir = root / "server"
        headers_dir = root / "headers"
        kb_http_dir = root / "kb" / "http"
        server_dir.mkdir(parents=True)
        headers_dir.mkdir(parents=True)
        kb_http_dir.mkdir(parents=True)
        (server_dir / "kb_client_extra.c").write_text(
            'const char *method = "kb.new_backdoor";\n'
            'const char *retired = "kb.chunks.store";\n',
            encoding="utf-8",
        )
        (server_dir / "kb_client_memory.c").write_text(
            'char *a = kb_directive_request("memory.new_backdoor", req);\n'
            'cJSON_AddStringToObject(req, "method", "index.new_backdoor");\n'
            'cJSON_AddStringToObject(req, "method", "v1.http");\n',
            encoding="utf-8",
        )
        (kb_http_dir / "kb_http_extra.c").write_text(
            'const char *route = "/v1/new-backdoor";\n',
            encoding="utf-8",
        )
        (root / "kb" / "kb_service.c").write_text(
            'if (strcmp(method->valuestring, "kb.search") == 0) return old(req);\n'
            'if (strcmp(method->valuestring, "bandit.export") == 0) return old(req);\n'
            'if (strcmp(method->valuestring, "server.info") == 0) return info(req);\n',
            encoding="utf-8",
        )
        (server_dir / "kb_client.c").write_text(
            '#include "kb_client_internal.h"\n',
            encoding="utf-8",
        )
        (root / "bad_internal_user.c").write_text(
            '#include "kb_client_internal.h"\n',
            encoding="utf-8",
        )
        (server_dir / "bad_root_scan.c").write_text(
            'void bad(void) { kb_client_canonical_index_scan("proj", "/repo", 1); }\n',
            encoding="utf-8",
        )
        (root / "bad_directive_caller.c").write_text(
            'void bad(void) { char *r = kb_directive_request("kb.export", req); }\n',
            encoding="utf-8",
        )
        (headers_dir / "kb_client.h").write_text(
            "char *kb_client_v1_post_json(const char *path, void *body, int timeout_ms, "
            "int *status_out);\n",
            encoding="utf-8",
        )

        planted = scan_methods(client_files(root))
        planted_directives = scan_legacy_directive_methods(client_files(root))
        routes = scan_routes(http_files(root))
        public_header_blocks_internal = public_client_header_exposed_symbols(root) == [
            "kb_client_v1_post_json"
        ]
        internal_header_offenders = scan_internal_client_header_includes(root)
        root_path_scan_offenders = scan_root_path_scan_users(root)
        retired_service_dispatch = scan_retired_service_rpc_dispatch(root)
        rogue_directive_offenders = scan_rogue_directive_calls(root)
        unexpected = planted - LEGACY_BASELINE
        unexpected_directives = planted_directives - LEGACY_DIRECTIVE_BASELINE
        stale = LEGACY_BASELINE - planted
        stale_directives = LEGACY_DIRECTIVE_BASELINE - planted_directives
        migrated_absent = not (MIGRATED_METHODS & LEGACY_BASELINE)
        if (
            unexpected == {"kb.new_backdoor", "kb.chunks.store"}
            and unexpected_directives == {"index.new_backdoor", "memory.new_backdoor"}
            and "/v1/new-backdoor" in routes
            and public_header_blocks_internal
            and internal_header_offenders == ["bad_internal_user.c"]
            and root_path_scan_offenders == ["server/bad_root_scan.c"]
            and retired_service_dispatch == ["bandit.export", "kb.search"]
            and rogue_directive_offenders == ["bad_directive_caller.c"]
            and stale == LEGACY_BASELINE
            and stale_directives == LEGACY_DIRECTIVE_BASELINE
            and migrated_absent
        ):
            print("kb-v1-coverage plant: ok")
            return 0
    print("kb-v1-coverage plant: failed", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Check kb-client RPC to /v1 migration coverage.")
    parser.add_argument("--src-dir", default="src", help="Source directory containing kb_client*.c")
    parser.add_argument("--openapi", help="OpenAPI v1 spec path")
    parser.add_argument("--plant-test", action="store_true", help="Run an internal baseline self-test")
    parser.add_argument("--verbose", action="store_true", help="Print current method-to-endpoint coverage")
    args = parser.parse_args()

    if args.plant_test:
        return plant_test()

    src_dir = pathlib.Path(args.src_dir)
    openapi_path = (pathlib.Path(args.openapi) if args.openapi
                    else pathlib.Path(__file__).resolve().parents[1] / "api" / "openapi-v1.yaml")
    methods = scan_methods(client_files(src_dir))
    directive_methods = scan_legacy_directive_methods(client_files(src_dir))
    routes = scan_routes(http_files(src_dir))

    unexpected = sorted(methods - LEGACY_BASELINE)
    unexpected_directives = sorted(directive_methods - LEGACY_DIRECTIVE_BASELINE)
    stale = sorted(LEGACY_BASELINE - methods)
    stale_directives = sorted(LEGACY_DIRECTIVE_BASELINE - directive_methods)
    unmapped = sorted(method for method in methods if method not in METHOD_ENDPOINTS)
    missing_routes = []
    for method in sorted(methods):
        for endpoint in METHOD_ENDPOINTS.get(method, ()):
            if not endpoint_route_available(endpoint, routes):
                missing_routes.append((method, endpoint))

    if unexpected:
        print("kb-v1-coverage: new legacy kb.* client methods found:", file=sys.stderr)
        for method in unexpected:
            print(f"  - {method}", file=sys.stderr)
        print("Add a /v1 endpoint and client migration instead of expanding the RPC surface.",
              file=sys.stderr)
        return 1

    if unexpected_directives:
        print("kb-v1-coverage: new legacy directive RPC methods found:", file=sys.stderr)
        for method in unexpected_directives:
            print(f"  - {method}", file=sys.stderr)
        print("Add a /v1 endpoint and client migration instead of expanding the directive RPC surface.",
              file=sys.stderr)
        return 1

    if unmapped:
        print("kb-v1-coverage: legacy methods missing migration mapping:", file=sys.stderr)
        for method in unmapped:
            print(f"  - {method}", file=sys.stderr)
        return 1
    if missing_routes:
        print("kb-v1-coverage: mapped /v1 routes missing:", file=sys.stderr)
        for method, endpoint in missing_routes:
            print(f"  - {method} -> {endpoint}", file=sys.stderr)
        return 1

    if stale:
        print("kb-v1-coverage: stale legacy baseline entries:", file=sys.stderr)
        for method in stale:
            print(f"  - {method}", file=sys.stderr)
        print("Remove stale entries from LEGACY_BASELINE so retired kb.* RPCs cannot be reintroduced.",
              file=sys.stderr)
        return 1

    if stale_directives:
        print("kb-v1-coverage: stale legacy directive baseline entries:", file=sys.stderr)
        for method in stale_directives:
            print(f"  - {method}", file=sys.stderr)
        print("Remove stale entries from LEGACY_DIRECTIVE_BASELINE as directive RPCs move to /v1.",
              file=sys.stderr)
        return 1

    openapi_rc = check_openapi(openapi_path)
    if openapi_rc != 0:
        return openapi_rc

    header_rc = check_public_client_header(src_dir)
    if header_rc != 0:
        return header_rc

    internal_header_rc = check_internal_client_header_includes(src_dir)
    if internal_header_rc != 0:
        return internal_header_rc

    root_path_scan_rc = check_root_path_scan_users(src_dir)
    if root_path_scan_rc != 0:
        return root_path_scan_rc

    retired_service_rpc_rc = check_retired_service_rpc_dispatch(src_dir)
    if retired_service_rpc_rc != 0:
        return retired_service_rpc_rc

    rogue_directive_rc = check_rogue_directive_calls(src_dir)
    if rogue_directive_rc != 0:
        return rogue_directive_rc

    if args.verbose:
        print("kb-v1-coverage: current legacy method baseline")
        for method in sorted(methods):
            endpoints = ", ".join(METHOD_ENDPOINTS[method])
            print(f"  {method} -> {endpoints}")
        print("kb-v1-coverage: current legacy directive RPC baseline")
        for method in sorted(directive_methods):
            print(f"  {method}")
        print("kb-v1-coverage: discovered /v1 routes")
        for route in sorted(routes):
            print(f"  {route}")

    print(
        "kb-v1-coverage: ok "
        f"({len(methods)} legacy kb.* methods pinned; "
        f"{len(directive_methods)} directive RPC methods pinned)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
