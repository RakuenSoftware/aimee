#!/usr/bin/env python3
"""Add a TESTS column to the grader: did the agent write a test that catches the defect?

Applies to ponytail-codex-benchmark/battery/codex_matrix_runner.py.

WHY. Grading ran exactly one file -- the upstream regression test -- copied over
whatever the agent had written in that path. Three consequences, all observed on
am_312e901904:

  1. An agent that writes NO test scores identically to one that writes a good
     one. ponytail-addon wrote none and passed.
  2. An agent that DOES write a test has it deleted before grading. aimee wrote a
     regression test that independently converged on the upstream engineer's
     strategy -- force the fd collision by dup2'ing a directory onto
     GIT_CRED_TOKEN_TARGET_FD -- and got zero credit for it.
  3. A ticket with two defects is scored on one. The reference commit changed
     src/posix/util.c, src/server/server_http_routes_git.c and the test; grading
     builds only the test. Both arms fixed defect 1, neither fixed defect 2
     (ponytail guessed HTTP caching, aimee guessed stale client state; the real
     bug was repos filed under the browsed owner rather than their own
     clone_url), and both scored a clean pass.

THE RULE, as specified: no tests => fail. Tests that do not test the defect =>
fail.

HOW, so it cannot be gamed. A test is only evidence if it FAILS on the broken
code. So the gate is red-green against the pristine corpus:

  RED   agent's test files, applied to the UNFIXED seed, must FAIL to build/run.
        A test that passes on broken code does not test the defect.
  GREEN the same files in the agent's own workspace, with its fix, must PASS.

Both required. An agent cannot satisfy RED by writing an assert(false) (it would
fail GREEN), nor GREEN by writing assert(true) (it would pass RED).

Deliberately NOT part of this gate: whether the agent's test resembles the
reference. Converging on the upstream approach is worth measuring and is
reported separately, but requiring it would grade style rather than behaviour.
"""
import ast
import py_compile
import re
import shutil
import sys
from pathlib import Path

RUNNER = Path("/opt/bench/ponytail-codex-benchmark/battery/codex_matrix_runner.py")

GATE = '''

# --- agent test gate -------------------------------------------------------
# Building a C test target is far slower than running one, so it does not share
# CHECK_TIMEOUT. Defined here rather than assumed: the gate is injected into a
# runner it does not own, and referencing a global that runner happens not to
# define fails at grading time, after the cell has already been paid for.
BUILD_TIMEOUT = int(os.environ.get("PT_BUILD_TIMEOUT", "900"))
# `.test.ts` / `.spec.tsx` is the dominant JS/TS convention and the original
# pattern only knew `_test.ts`. On am_312e901904 that made the agent's
# frontend/src/setup/ownerUrl.test.ts invisible to the gate, which would
# have scored a two-defect fix as if only the C half carried a test.
TEST_PATH_RE = re.compile(
    r"(^|/)tests?/"                      # tests/ or test/ directory
    r"|(^|/)test_[^/]+$"                 # test_foo.py
    r"|_test\\.(go|py|ts|tsx|js|jsx)$"    # foo_test.go
    r"|\\.(test|spec)\\.[jt]sx?$"          # foo.test.ts, foo.spec.tsx
)


def agent_test_files(changed):
    """Test files the agent added or modified, from the cell's changed list."""
    return [p for p in changed if TEST_PATH_RE.search(p)]


def _hidden_spec(task):
    """TEST_FILES / TEST_TARGET declared by this task's graded test module.

    Reused rather than guessed: the module already names the build target for
    the file the reference commit tested, so an agent editing that same file is
    built and run exactly as the reference is.
    """
    text = (HIDDEN / f"{task}.py").read_text()
    files = re.search(r"^TEST_FILES\\s*=\\s*(\\[[^\\]]*\\])", text, re.M)
    target = re.search(r'^TEST_TARGET\\s*=\\s*"([^"]+)"', text, re.M)
    return (eval(files.group(1)) if files else []), (target.group(1) if target else None)


def _build_and_run(ws, target):
    """Build one test target and run it. Returns (built, passed).

    run() passes timeout= to subprocess.run, which RAISES TimeoutExpired --
    check=False does not suppress it. An unhandled raise here kills the whole
    cell at grading time, after the agent has already run. A build that times
    out is simply "did not build"; it must never destroy the measurement.
    """
    try:
        build = run(["make", "-C", "src", target, "-j8"], cwd=ws, check=False,
                    timeout=BUILD_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, False
    binary = Path(ws) / "src" / target
    if build.returncode != 0 or not binary.is_file():
        return False, False
    try:
        proc = run([str(binary)], cwd=ws, check=False, timeout=CHECK_TIMEOUT)
    except subprocess.TimeoutExpired:
        return True, False
    return True, proc.returncode == 0


def _test_function_names(text):
    """Test entry points declared in a file, across the corpus's languages."""
    names = set()
    names |= set(re.findall(r"^\\s*(?:static\\s+)?void\\s+(test_\\w+)\\s*\\(", text, re.M))  # C
    names |= set(re.findall(r"^\\s*def\\s+(test_\\w+)\\s*\\(", text, re.M))                  # py
    q = chr(34) + chr(39)   # both quote styles, without either appearing here
    names |= set(re.findall(
        "(?:^|[^A-Za-z_])(?:it|test)[(][ ]*[" + q + "]([^" + q + "]+)", text, re.M))
    return names


def _assertions(text):
    """Assertion statements in a test file, normalised for whitespace.

    Deliberately shallow. The question is not "is this the same assertion" but
    "did an assertion that used to be here stop being here", which a normalised
    literal comparison answers without needing to parse four languages.
    """
    out = set()
    for pat in (r"\\bassert\\s*\\(.*?\\)\\s*;",          # C / C++
                r"\\bassert\\s+[^\\n]+",                  # python bare assert
                r"\\bself\\.assert\\w+\\s*\\(.*?\\)",     # unittest
                r"\\bexpect\\s*\\(.*?\\)\\s*\\.[^\\n;]+"):  # jest / vitest
        for m in re.findall(pat, text, re.S):
            out.add(" ".join(m.split()))
    return out


def weakened_existing_assertions(workspace, seed, written):
    """Assertions that existed in the PRISTINE test and are gone from the agent's.

    removed_existing_tests() only sees DELETED test functions. On am_270b3483d5
    the agent instead inverted assertions inside an existing test that has no
    named test functions at all -- it is a bare main() -- so nothing was
    "removed" and the check could not fire. It flipped

        assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_READY);

    to expect NO_TRUST_BUNDLE, matching its own stricter behaviour, and scored
    catches_defect for it.

    Editing an existing test to agree with your change is not evidence that a
    defect was caught. Adding NEW assertions is untouched by this: only
    assertions that DISAPPEARED are reported.
    """
    lost = {}
    for rel in written:
        before, after = seed / rel, Path(workspace) / rel
        if not before.is_file() or not after.is_file():
            continue
        try:
            gone = _assertions(before.read_text()) - _assertions(after.read_text())
        except (OSError, UnicodeDecodeError):
            continue
        if gone:
            lost[rel] = sorted(gone)[:5]
    return lost


def removed_existing_tests(workspace, seed, written):
    """Test entry points that existed in the PRISTINE tree and are gone now.

    Red-green cannot see this on its own. Inverting an existing assertion fails
    on pristine code and passes on the changed code, which satisfies BOTH halves
    of the gate -- so "I rewrote the guard to match my change" scores identically
    to "I wrote a test that catches the defect".

    Observed on am_b84c9294aa: the agent deleted the `waiters > 0` prerequisite
    from the pool's starvation detector -- the guard that stops a BUSY pool being
    restarted under its users -- and rewrote
    test_busy_pool_is_not_treated_as_starved (asserts starved_calls == 0) into
    test_stuck_pool_without_waiters_gives_up (asserts == 1). The gate scored it
    catches_defect; the graded suite, which still contains the original, failed.

    Deleting a test is a legitimate thing for an engineer to do. It is not
    evidence that a defect was caught, which is the only thing this column
    claims.
    """
    lost = {}
    for rel in written:
        before, after = seed / rel, Path(workspace) / rel
        if not before.is_file() or not after.is_file():
            continue  # a NEW test file cannot have removed anything
        try:
            gone = _test_function_names(before.read_text()) - _test_function_names(after.read_text())
        except (OSError, UnicodeDecodeError):
            continue
        if gone:
            lost[rel] = sorted(gone)
    return lost


def agent_test_gate(workspace, task, changed, hidden_ok=False):
    """Did the agent write a test that actually catches this defect?

    no test           -> fail, reason "no_test"
    passes on broken  -> fail, reason "does_not_catch_defect"
    fails on fixed    -> fail, reason "fails_on_own_fix"

    hidden_ok is the GRADED suite's verdict and arbitrates the integrity checks
    below. Removing a test, or dropping an assertion, is only evidence of gaming
    when upstream's own test ALSO fails. When a ticket asks for behaviour to
    change, the assertions that pinned the old behaviour must change with it, and
    refusing to let them is penalising the correct fix.
    """
    written = agent_test_files(changed)
    if not written:
        return {"ok": False, "reason": "no_test", "files": []}

    graded_files, target = _hidden_spec(task)
    if not target:
        return {"ok": False, "reason": "no_build_target_for_task", "files": written}

    # GREEN: the agent's test must pass against the agent's own fix.
    built, green = _build_and_run(workspace, target)
    if not built:
        return {"ok": False, "reason": "agent_test_does_not_build", "files": written}

    # RED: the same test files, on the PRISTINE corpus, must fail.
    #
    # A missing seed is a HARNESS condition, not an agent failure. Raising here
    # killed the whole cell after the agent had already run; report it instead
    # so the run survives and the cause is legible in the results.
    seed = seed_for(task)
    if not seed.is_dir():
        return {"ok": False, "reason": "seed_unavailable_for_red_replay:%s" % seed,
                "files": written}
    # NOT the default /tmp: on this host that is a tmpfs, i.e. RAM. Copying a
    # 246MB tree there and running a full C build inside it competes with the
    # services under measurement for the box's 8GB. Stage beside the seed,
    # which is real disk, and fall back to the default only if that is unset.
    scratch = seed.parent if seed.parent.is_dir() else None
    with tempfile.TemporaryDirectory(prefix="aimee-redgate-", dir=scratch) as tmp:
        pristine = Path(tmp) / "ws"
        shutil.copytree(seed, pristine, symlinks=True)
        copied = 0
        for rel in written:
            src = Path(workspace) / rel
            if not src.is_file():
                continue
            dest = pristine / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
            copied += 1
        if not copied:
            return {"ok": False, "reason": "no_test_files_to_replay", "files": written}
        red_built, red_passed = _build_and_run(pristine, target)

    # A test that does not compile against the unfixed tree still counts as red:
    # it is referencing the fix's own API, which is a legitimate way to catch a
    # defect, and it cannot be used to fake a pass because GREEN still applies.
    caught = (not red_built) or (not red_passed)
    if not caught:
        return {"ok": False, "reason": "does_not_catch_defect", "files": written,
                "red_passed": True, "green_passed": green}
    if not green:
        return {"ok": False, "reason": "fails_on_own_fix", "files": written,
                "red_passed": False, "green_passed": False}
    # Integrity checks, arbitrated by the graded suite.
    #
    # Measured on am_e4c4afa194: the ticket requires the ledger command field to
    # carry a one-way fingerprint instead of the raw memory key, so the existing
    # assertion that it equals "fact/ReleasePlan" HAD to go. All four arms
    # removed exactly that assertion, all four passed the graded suite, and the
    # unconditional check failed all four. A check that fires on 100% of arms
    # while every one of them passes grading is measuring itself, not them.
    #
    # When the graded suite FAILS, the same edit is the tell (am_b84c9294aa,
    # am_270b3483d5): the agent changed the test to agree with a change upstream
    # rejects.
    lost = removed_existing_tests(workspace, seed, written)
    weakened = weakened_existing_assertions(workspace, seed, written)
    if lost and not hidden_ok:
        return {"ok": False, "reason": "removed_existing_test:%s" % json.dumps(lost),
                "files": written, "red_passed": False, "green_passed": True}
    if weakened and not hidden_ok:
        return {"ok": False, "reason": "changed_existing_assertions:%s" % json.dumps(weakened),
                "files": written, "red_passed": False, "green_passed": True}
    note = ""
    if lost or weakened:
        # Still recorded: the edit happened and is worth seeing, it just is not
        # disqualifying when the graded contract is satisfied.
        note = ";edited_existing_tests_but_graded_ok"
    return {"ok": True, "reason": "catches_defect" + note, "files": written,
            "red_passed": False, "green_passed": True}
# --- end agent test gate ---------------------------------------------------
'''


def unresolved_globals(gate_src, runner_src):
    """Free names in `gate_src` that `runner_src` does not define.

    The gate is injected into a runner it does not own, so it can only use
    globals that runner already provides. Referencing one it does not -- as
    BUILD_TIMEOUT did -- raises NameError at GRADING time, i.e. after the agent
    has run and the cell has been paid for. py_compile cannot see it, so the
    names are resolved statically here instead.
    """
    import builtins
    import symtable

    free = set()
    for scope in _walk_scopes(symtable.symtable(gate_src, "<gate>", "exec")):
        for sym in scope.get_symbols():
            if sym.is_global() and not sym.is_assigned():
                free.add(sym.get_name())

    try:
        defined = _module_level_names(ast.parse(runner_src))
    except SyntaxError as exc:
        return ["runner does not parse: %s" % exc]
    defined |= set(dir(builtins))
    # Names the gate defines for itself at module level.
    defined |= _module_level_names(ast.parse(gate_src))

    missing = sorted(n for n in free if n not in defined)
    return ["gate references `%s`, which the runner does not define" % n for n in missing]


def _walk_scopes(scope):
    yield scope
    for child in scope.get_children():
        yield from _walk_scopes(child)


def _module_level_names(tree):
    names = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    names.add(target.id)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                names.add((alias.asname or alias.name).split(".")[0])
    return names


def main():
    text = RUNNER.read_text()
    if "agent_test_gate" in text:
        print("already patched")
        return 0
    RUNNER.with_suffix(".py.pre-testgate.bak").write_text(text)

    for mod in ("import tempfile", "import re", "import subprocess", "import json"):
        if mod not in text:
            text = text.replace("import shutil", mod + "\nimport shutil", 1)

    anchor = "def hidden_test(workspace: Path, task: str) -> dict:"
    text = text.replace(anchor, GATE.strip() + "\n\n\n" + anchor, 1)

    # Call it AFTER collect_patch, which is what binds `changed`. Anchoring on
    # hidden_test instead put the call one line above that binding, so every
    # cell died with "cannot access local variable 'changed'" -- after the agent
    # had already run, so the whole cell's credits were spent and thrown away.
    #
    # The gate reads the agent's own copy from `workspace`, the untouched
    # execution workspace, so moving it below collect_patch does not expose it
    # to the graded test file that hidden_test swaps into graded_ws.
    anchor_changed = "    changed, loc = collect_patch(graded_ws, artifact)"
    if anchor_changed not in text:
        print("FATAL: collect_patch anchor not found; refusing to patch blind")
        return 2
    # Wrap it. The gate is a SCORING column: if it raises, it must cost that
    # column, never the cell. Three cells were lost to exceptions raised inside
    # grading after the agent had already run to completion -- the expensive
    # part was already paid for and was thrown away by a reporting bug.
    text = text.replace(
        anchor_changed,
        anchor_changed + "\n"
        "    try:\n"
        "        tests = agent_test_gate(workspace, task, changed, hidden.get(\"passed\", False))\n"
        "    except Exception as exc:  # noqa: BLE001 - a scoring bug must not void the cell\n"
        "        tests = {\"ok\": False, \"files\": [],\n"
        "                 \"reason\": \"gate_error:%s: %s\" % (type(exc).__name__, exc)}", 1)

    text = text.replace(
        '        "hidden_ok": hidden["passed"],',
        '        "hidden_ok": hidden["passed"],\n'
        '        "tests_ok": tests["ok"],\n'
        '        "tests_reason": tests["reason"],\n'
        '        "tests_files": tests.get("files", []),', 1)

    RUNNER.write_text(text)

    # Verify before anyone spends a cell on it. The ordering bug that motivated
    # this cost a full agent run: the cell executed, then grading raised
    # "cannot access local variable 'changed'" and the whole thing was binned.
    # py_compile would not have caught that one (it is a runtime unbound-local),
    # so check the ordering explicitly as well.
    problems = []
    try:
        py_compile.compile(str(RUNNER), doraise=True)
    except py_compile.PyCompileError as exc:
        problems.append("does not compile: %s" % exc)

    # Every global the gate reads must exist in the runner it is injected into.
    # BUILD_TIMEOUT did not, and nothing caught it until grading -- one more
    # cell run to completion and then discarded. A NameError inside a function
    # is invisible to py_compile, so resolve the free names explicitly.
    problems += unresolved_globals(GATE, RUNNER.read_text())

    body = RUNNER.read_text()
    bind = body.find("changed, loc = collect_patch")
    use = body.find("tests = agent_test_gate(")
    if bind < 0 or use < 0:
        problems.append("expected both collect_patch and agent_test_gate call sites")
    elif use < bind:
        problems.append("agent_test_gate is called before `changed` is bound")

    if problems:
        RUNNER.write_text(RUNNER.with_suffix(".py.pre-testgate.bak").read_text())
        for p in problems:
            print("FATAL:", p)
        print("reverted:", RUNNER)
        return 3

    print("patched:", RUNNER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
