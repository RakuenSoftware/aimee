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
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Each daemon's module-client surface: the one file where it registers what its
# own code paths need. Both exist for the same reason and neither may stand in
# for the other.
ADAPTERS = {
    "kb": ROOT / "src/kb/kb_module_stage_adapters.c",
    "server": ROOT / "src/server/module_stage_adapters.c",
}

# The Make target each daemon is linked by. What it builds is that target's
# link graph, read from make itself, so the gate sees exactly what ships.
TARGETS = {
    "kb": "../aimee-kb",
    "server": "../aimee-server",
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
    # aimee-server links the KB store sources, but on its link every .text
    # section of these owners is removed by --gc-sections (seen with
    # -Wl,--print-gc-sections on a -fno-lto link): no server path reaches them,
    # so their providers are never consulted there.
    ("server", "aimee_kb_store_register_css_analysis_providers"):
        "modules/kb/c/canonical_index.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_css_render_compare_provider"):
        "modules/kb/c/css_render.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_embed_provider"):
        "modules/kb/c/kb_payload.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_identity_key_provider"):
        "modules/kb/c/kb_store_tenant.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_vault_crypto_provider"):
        "modules/kb/c/vault_pg.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_vault_reseal_provider"):
        "modules/kb/c/org_vault_rewrap.c is linked into aimee-server, and the linker removes all of it.",
    ("server", "aimee_kb_store_register_vault_witness_provider"):
        "modules/kb/c/kb_store_witness_checkpoint.c is linked into aimee-server, and the linker removes all of it.",
}


def linked_sources(root, targets):
    """The .c files each target links, from make's own database (-pn: no build).

    Archives are followed to their members, so a source linked through a
    static library counts the same as one named directly; generated objects
    with no source are skipped."""
    out = subprocess.run(
        ["make", "-pn", "--no-print-directory", *targets.values()],
        cwd=root / "src", capture_output=True, text=True, timeout=300).stdout
    rules = {}
    for line in out.splitlines():
        head, sep, prereqs = line.partition(": ")
        if sep and not head.startswith(("#", "\t")) and " " not in head and ":=" not in prereqs:
            rules.setdefault(head, prereqs.split("|")[0].split())

    def sources(target, seen):
        found = set()
        for prereq in rules.get(target, ()):
            if prereq in seen:
                continue
            seen.add(prereq)
            if prereq.endswith(".a"):
                found |= sources(prereq, seen)
            elif prereq.startswith("build/obj/") and prereq.endswith(".o"):
                source = object_source(prereq[len("build/obj/"):-2])
                if source:
                    found.add(source)
        return found

    def object_source(stem):
        # Some daemons compile their own copy of a source under a prefix (the
        # KB builds modules/x/y.c as build/obj/kb/modules/x/y.o), so strip
        # leading directories until the path names a real source.
        parts = stem.split("/")
        for start in range(len(parts)):
            candidate = "/".join(parts[start:]) + ".c"
            if (root / "src" / candidate).is_file():
                return candidate
        return None

    return {daemon: sources(target, set()) for daemon, target in targets.items()}


def analyse(root, built, adapters, unreachable):
    """Returns (exit_code, [messages]). Pure over its inputs so it can be tested.

    built maps each daemon to the src-relative .c files it links."""
    for daemon, files in built.items():
        if not files:
            return 2, [f"no sources resolved for '{daemon}'; its Make link target has moved"]

    registered = {}
    for daemon, path in adapters.items():
        if not path.exists():
            return 2, [f"missing {path}"]
        registered[daemon] = set(REGISTRAR_CALL.findall(path.read_text(encoding="utf-8")))

    # Where each registrar is defined -- that file owns the function pointer, and
    # whatever else lives in it is the consumer that goes null without it.
    owner = {}
    for source in (root / "src").glob("**/*.c"):
        if source in adapters.values():
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for name in REGISTRAR_DEF.findall(text):
            owner.setdefault(name, set()).add(str(source.relative_to(root / "src")))

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
            if (daemon, registrar) in unreachable:
                seen_exempt.add((daemon, registrar))
                continue
            failures.append(
                f"  {daemon} builds {sorted(owners & files)[0]}, which owns {registrar}, "
                f"but {adapters[daemon]} never registers it")

    if failures:
        return 1, ["a daemon builds a provider's consumer and never registers the provider.",
                   "The pointer stays null, the path fails closed, and no test notices because "
                   "every test registers its own.",
                   "Register it in that daemon's adapter, or record in UNREACHABLE why the code "
                   "cannot run there."] + failures

    stale = set(unreachable) - seen_exempt
    if stale:
        return 1, ["UNREACHABLE lists entries that no longer apply; remove them:"] + [
            f"  {daemon} {registrar}" for daemon, registrar in sorted(stale)]

    # A gate that cannot see anything must not report success. If no provider
    # pair resolved at all, the parse has drifted away from the tree and this
    # check has quietly stopped checking.
    if checked == 0:
        return 2, ["no provider/daemon pairs resolved at all; the adapters or the registrar "
                   "spelling have moved and this gate is no longer checking anything"]

    return 0, [f"ok ({len(adapters)} daemons, {checked} provider/daemon pairs, "
               f"{len(unreachable)} recorded unreachable)"]


def main() -> int:
    code, messages = analyse(ROOT, linked_sources(ROOT, TARGETS), ADAPTERS, UNREACHABLE)
    stream = sys.stdout if code == 0 else sys.stderr
    for line in messages:
        print(f"check-provider-registration: {line}" if line is messages[0] else line, file=stream)
    return code


if __name__ == "__main__":
    sys.exit(main())
