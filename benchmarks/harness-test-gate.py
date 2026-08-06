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
import py_compile
import re
import shutil
import sys
from pathlib import Path

RUNNER = Path("/opt/bench/ponytail-codex-benchmark/battery/codex_matrix_runner.py")

GATE = '''

# --- agent test gate -------------------------------------------------------
TEST_PATH_RE = re.compile(r"(^|/)tests?/|(^|/)test_[^/]+$|_test\\.(go|py|ts|tsx)$")


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
    """Build one test target and run it. Returns (built, passed)."""
    build = run(["make", "-C", "src", target, "-j8"], cwd=ws, check=False,
                timeout=BUILD_TIMEOUT)
    binary = Path(ws) / "src" / target
    if build.returncode != 0 or not binary.is_file():
        return False, False
    proc = run([str(binary)], cwd=ws, check=False, timeout=CHECK_TIMEOUT)
    return True, proc.returncode == 0


def agent_test_gate(workspace, task, changed):
    """Did the agent write a test that actually catches this defect?

    no test           -> fail, reason "no_test"
    passes on broken  -> fail, reason "does_not_catch_defect"
    fails on fixed    -> fail, reason "fails_on_own_fix"
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
    seed = seed_for(task)
    with tempfile.TemporaryDirectory(prefix="aimee-redgate-") as tmp:
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
    return {"ok": True, "reason": "catches_defect", "files": written,
            "red_passed": False, "green_passed": True}
# --- end agent test gate ---------------------------------------------------
'''


def main():
    text = RUNNER.read_text()
    if "agent_test_gate" in text:
        print("already patched")
        return 0
    RUNNER.with_suffix(".py.pre-testgate.bak").write_text(text)

    for mod in ("import tempfile", "import re"):
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
    text = text.replace(
        anchor_changed,
        anchor_changed + "\n"
        "    tests = agent_test_gate(workspace, task, changed)", 1)

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
