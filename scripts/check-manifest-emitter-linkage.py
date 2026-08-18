#!/usr/bin/env python3
"""A test target that links a caller must link what the caller calls.

The served-manifest work added one small object per served body -- the command
catalogue, the dispatch rows, the no-arg list, the argument specs, the spec
interpreter -- and every single one broke the same way:

    undefined reference to `cli_dispatch_defs_to_json'
    undefined reference to `cli_marshal_defs_to_json'
    undefined reference to `cli_argspec_defs_to_json'
    undefined reference to `cli_argspec_build'

Four times, always found by CI minutes after a push, because tests/Rules.mk
maintains its prerequisite lists by hand and nothing relates them to what the
code actually calls. The last one needed edits to ten separate targets.

So derive it. For each provider below, find the functions it exports and the
objects whose sources call them; then require every Rules.mk target linking a
calling object to link the provider too. Adding a provider function, or a new
caller, now fails at lint naming the target -- not in a build naming a symbol.

Scoped to these providers on purpose: they are small leaf objects that exist to
be linked alongside their caller, which is exactly the shape that keeps being
forgotten. A whole-program link check is a different tool (check-linking).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RULES = ROOT / "src/tests/Rules.mk"

# provider object -> the source that defines its exported functions.
PROVIDERS = {
    "server/cli_command_defs.o": "src/server/cli_command_defs.c",
    "server/cli_dispatch_defs.o": "src/server/cli_dispatch_defs.c",
    "server/cli_marshal_defs.o": "src/server/cli_marshal_defs.c",
    "server/cli_argspec_defs.o": "src/server/cli_argspec_defs.c",
    "cli_argspec.o": "src/cli_argspec.c",
}

# Sources that might call them, mapped to the object they compile to. Kept
# explicit: these are the two translation units the manifest work touches, and
# guessing an object name from a path is how a check quietly stops matching.
CALLERS = {
    "src/server/server_http_config_routes.c": "server/server_http_config_routes.o",
    "src/cli_v1_routes_b.c": "cli_v1_routes_b.o",
}

TESTS_DIR = ROOT / "src/tests"

_RULES_TEXT = None  # plant-test override


def exported_functions(src_path):
    """Non-static functions the provider defines, by name."""
    text = (ROOT / src_path).read_text(encoding="utf-8", errors="replace")
    names = set()
    for m in re.finditer(r"^(?!static)([A-Za-z_][\w \*]*?)\b([a-z_][\w]*)\s*\([^;]*?\)\s*$",
                         text, re.M):
        names.add(m.group(2))
    if not names:
        raise SystemExit(f"check-manifest-emitter-linkage: no exported functions found in "
                         f"{src_path}; the extractor has drifted from the source")
    return names


def required_edges():
    """(caller object, provider object) pairs the link lists must honour."""
    edges = set()
    for src, caller_obj in CALLERS.items():
        body = (ROOT / src).read_text(encoding="utf-8", errors="replace")
        for provider_obj, provider_src in PROVIDERS.items():
            if provider_src.endswith(pathlib.Path(src).name):
                continue  # a source does not depend on itself
            for fn in exported_functions(provider_src):
                if re.search(rf"\b{re.escape(fn)}\s*\(", body):
                    edges.add((caller_obj, provider_obj))
                    break
    if not edges:
        raise SystemExit("check-manifest-emitter-linkage: found no caller->provider edges; "
                         "the extractor has drifted from the sources")
    return sorted(edges)


def including_tests(caller_src):
    """Test sources that #include a caller source instead of linking its object.

    A test that includes the source to reach a static function does NOT link the
    caller's object -- doing both would duplicate every symbol in it -- but it
    still inherits the calls, so it needs the provider all the same.

    This is the case the first version of this check was blind to. It looked
    only for targets LINKING the caller object, reported ok, and the very next
    unit-test run failed on `cli_argspec_build` in a target that includes
    cli_v1_routes_b.c. A check that only understands one of the two ways to
    depend on code will keep passing while the build breaks.
    """
    name = pathlib.Path(caller_src).name
    out = set()
    for src in sorted(TESTS_DIR.glob("*.c")):
        body = src.read_text(encoding="utf-8", errors="replace")
        if re.search(rf'#\s*include\s+"[^"]*{re.escape(name)}"', body):
            out.add(f"tests/{src.stem}.o")
    return out


def rules(text):
    """(target, prerequisites) per rule, with continuations joined."""
    joined = re.sub(r"\\\n\s*", " ", text)
    out = []
    for line in joined.split("\n"):
        if ":" not in line or not line.startswith("$("):
            continue
        target, _, prereqs = line.partition(":")
        out.append((target.strip(), prereqs))
    return out


def main():
    plant = "--plant-test" in sys.argv
    text = _RULES_TEXT if _RULES_TEXT is not None else RULES.read_text(encoding="utf-8")
    if plant:
        # Drop one provider object everywhere and require the check to notice.
        # Removing it EVERYWHERE also exercises the include-style dependants,
        # which the first version of this check could not see at all.
        text = text.replace("$(OBJDIR)/cli_argspec.o ", "")

    edges = required_edges()
    # A target depends on a caller either by linking its object or by linking a
    # test object whose source #includes it. Both inherit the calls.
    caller_objs = {}
    for caller_obj, provider_obj in edges:
        src = next(s for s, o in CALLERS.items() if o == caller_obj)
        caller_objs.setdefault(caller_obj, {caller_obj} | including_tests(src))

    problems = []
    checked = 0
    for target, prereqs in rules(text):
        for caller_obj, provider_obj in edges:
            if not any(f"$(OBJDIR)/{o}" in prereqs for o in caller_objs[caller_obj]):
                continue
            checked += 1
            if f"$(OBJDIR)/{provider_obj}" not in prereqs:
                problems.append(f"  {target}\n      depends on {caller_obj} (linked, or "
                                f"#included by one of its test sources) but does not link "
                                f"{provider_obj}, which it calls into")

    if plant:
        if problems:
            print("check-manifest-emitter-linkage: plant-test ok "
                  "(a removed provider object was caught)")
            return 0
        print("check-manifest-emitter-linkage: PLANT FAIL - removing a provider object did NOT "
              "fail the check; it is decoration", file=sys.stderr)
        return 1

    if problems:
        print("check-manifest-emitter-linkage: FAIL", file=sys.stderr)
        print("\n".join(sorted(set(problems))), file=sys.stderr)
        return 1

    print(f"check-manifest-emitter-linkage: ok ({len(edges)} caller->provider edges, "
          f"{checked} target linkages verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
