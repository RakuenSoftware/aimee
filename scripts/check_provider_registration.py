#!/usr/bin/env python3
"""A daemon that BUILDS a provider's consumer must REGISTER that provider.

Written for a gap that had shipped. Signal capture is served by aimee-kb -- that
is where the learning tables live -- and the router it calls needs a signal
classifier to decide which sinks a signal reaches. Only aimee-server ever
registered one. In the KB the function pointer was null, so every signal was
refused with a single WARN while the route answered 200 carrying an error
document and wrote nothing. Signal ingest through the KB had never worked.

That is the sibling of the gap check-module-placement.py guards. There the
module was absent; here the module was present and nothing wired it up. Both
fail the same way: fail-closed, silent, and invisible to every test, because
each test registers its own provider and so can never notice that production
does not.

The rule: for each provider registrar, find the file that owns its function
pointer. Any daemon that builds that file must register the provider in its own
adapter -- or record here why the code is unreachable in that daemon, with the
evidence. An entry cannot outlive its reason: a stale one is reported.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

# Each daemon's module-client surface: the one file where it registers what its
# own code paths need. Both exist for the same reason and neither may stand in
# for the other.
ADAPTERS = {
    "kb": ROOT / "src/kb/kb_module_stage_adapters.c",
    "server": ROOT / "src/server/module_stage_adapters.c",
}

# Which CMake source lists each daemon links.
SOURCE_LISTS = {
    "kb": ("KB_SRCS", "KB_DATA_SRCS", "KB_CORE_SRCS", "DB2_SRCS", "DB2_HOST_ADAPTER_SRCS"),
    "server": ("SERVER_SRCS", "SERVER_DATA_SRCS", "SERVER_CORE_SRCS", "DATA_SRCS", "CORE_SRCS",
               "AGENT_SRCS", "CMD_SRCS", "GIT_SRCS", "DB1_SRCS"),
}

REGISTRAR_DEF = re.compile(r"^\s*void\s+([a-z0-9_]+_register_[a-z0-9_]+)\s*\(", re.M)
REGISTRAR_CALL = re.compile(r"\b([a-z0-9_]+_register_[a-z0-9_]+)\s*\(")

# A provider whose owner a daemon links but never reaches. Each entry carries
# the evidence that settled it -- linking a file is not reaching it, and
# --gc-sections drops what nothing calls.
#
# key: (daemon, registrar) -> why it is unreachable there
UNREACHABLE = {
    ("kb", "ws_scope_register_ref_validator"):
        "workspace_scope.c is linked, but its only consumers (modules/git/git_project.c and the "
        "webuser files) are not in the KB build. It also fails CLOSED -- a null validator rejects "
        "every ref -- so it could never have been a silent accept.",
    ("kb", "wfe_advance_register_decision_provider"):
        "wfe_advance.c is linked, but wfe_engine.c and wfe_advance_exec.c are not in the KB build, "
        "so nothing drives the advance path there.",
    ("server", "kb_route_acl_register_authorization_provider"):
        "kb/http/kb_route_acl.c reaches its provider only from kb_http.c's console-admin branch, "
        "and that branch is absent from the linked aimee-server binary: 'control-web authorization "
        "unavailable' appears in aimee-kb and not in aimee-server.",
    ("server", "kb_curator_grounding_register_provider"):
        "kb_curator_grounding.c is reached only from kb_curator_extract_code.c, which is absent "
        "from the linked aimee-server binary: 'sidecar temp path too long to quote safely' appears "
        "in aimee-kb and not in aimee-server.",
    ("kb", "agent_tools_register_classifier"):
        "posix/agent_runtime.c is in the KB build and calls dispatch_tool_call_ctx, but the path is "
        "absent from the linked aimee-kb binary: strings unique to it ('error: spill store "
        "unavailable', 'git tools are not available on this surface') appear in aimee-server and "
        "not in aimee-kb, so --gc-sections dropped it because nothing reaches it.",
}


def expand(name, seen=None):
    """The .c files named by one set(NAME ...) block, following ${REFERENCES}."""
    seen = seen or set()
    if name in seen:
        return set()
    seen.add(name)
    match = re.search(r"^set\(" + name + r"\b(.*?)^\)", CMAKE, re.S | re.M)
    if not match:
        return set()
    body = match.group(1)
    files = set(re.findall(r"\$\{AIMEE_SRC_DIR\}/(\S+\.c)", body))
    for ref in re.findall(r"\$\{([A-Z0-9_]+)\}", body):
        files |= expand(ref, seen)
    return files


def main() -> int:
    built = {d: set().union(*(expand(l) for l in lists)) for d, lists in SOURCE_LISTS.items()}
    for daemon, files in built.items():
        if not files:
            print(f"check-provider-registration: no sources resolved for '{daemon}'; "
                  "the CMake list names have moved", file=sys.stderr)
            return 2

    registered = {}
    for daemon, path in ADAPTERS.items():
        if not path.exists():
            print(f"check-provider-registration: missing {path}", file=sys.stderr)
            return 2
        registered[daemon] = set(REGISTRAR_CALL.findall(path.read_text(encoding="utf-8")))

    # Where each registrar is defined -- that file owns the function pointer, and
    # whatever else lives in it is the consumer that goes null without it.
    owner = {}
    for source in ROOT.glob("src/**/*.c"):
        if source in ADAPTERS.values():
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for name in REGISTRAR_DEF.findall(text):
            owner.setdefault(name, set()).add(str(source.relative_to(ROOT / "src")))

    # Only module-stage providers are in scope: the ones some daemon registers
    # through its adapter. A registrar no adapter mentions is an ordinary
    # in-process hook wired up wherever it belongs -- tool tables, workflow
    # block executors, config reappliers -- and demanding it here would be noise
    # loud enough to bury the one finding that matters.
    in_scope = set().union(*registered.values())

    failures, seen_exempt, checked = [], set(), 0
    for registrar, owners in sorted(owner.items()):
        if registrar not in in_scope:
            continue
        for daemon, files in built.items():
            if not (owners & files):
                continue  # this daemon does not build the consumer
            checked += 1
            if registrar in registered[daemon]:
                continue
            if (daemon, registrar) in UNREACHABLE:
                seen_exempt.add((daemon, registrar))
                continue
            failures.append(
                f"  {daemon} builds {sorted(owners & files)[0]}, which owns {registrar}, "
                f"but {ADAPTERS[daemon].relative_to(ROOT)} never registers it")

    if failures:
        print("check-provider-registration: a daemon builds a provider's consumer and never "
              "registers the provider.", file=sys.stderr)
        print("The pointer stays null, the path fails closed, and no test notices because every "
              "test registers its own.", file=sys.stderr)
        print("Register it in that daemon's adapter, or record in UNREACHABLE why the code cannot "
              "run there.", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    stale = set(UNREACHABLE) - seen_exempt
    if stale:
        print("check-provider-registration: UNREACHABLE lists entries that no longer apply; "
              "remove them:", file=sys.stderr)
        for daemon, registrar in sorted(stale):
            print(f"  {daemon} {registrar}", file=sys.stderr)
        return 1

    print(f"check-provider-registration: ok ({len(ADAPTERS)} daemons, {checked} provider/daemon "
          f"pairs, {len(UNREACHABLE)} recorded unreachable)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
