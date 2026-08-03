"""The CI change classifier: docs_only vs docs_changed.

Extracted from .github/workflows/ci.yml and run as a shell script, rather than copied,
so it cannot drift from what CI executes -- which is the failure this test exists to
prevent a second time.

The distinction is load-bearing. docs_only decides whether to SKIP the expensive gate;
docs_changed decides whether to RUN the documentation check. Conflating them meant a
pull request touching docs plus one other file had its documentation checked by nothing,
and six em-dash violations reached testing that way (#2280).
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CI = ROOT / ".github" / "workflows" / "ci.yml"


def extract_classifier():
    """Pull the classify step's shell body out of ci.yml."""
    text = CI.read_text(encoding="utf-8")
    m = re.search(r"id: classify\n(.*?)\n  [a-z-]+:", text, re.S)
    if not m:
        sys.exit("could not find the classify step in ci.yml")
    block = m.group(1)
    run = re.search(r"run: \|\n(.*)", block, re.S)
    if not run:
        sys.exit("classify step has no run: block")
    body = run.group(1)
    # Strip the YAML block indentation.
    lines = [l[10:] if l.startswith(" " * 10) else l for l in body.splitlines()]
    return "\n".join(lines)


BODY = extract_classifier()


def classify(paths, event="pull_request"):
    """Run the real classifier over a NUL-separated file list."""
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "out"
        out.touch()
        script = pathlib.Path(d) / "classify.sh"
        # The classifier reads the changed files from a `git diff -z` on stdin; feed the
        # same shape without needing a repository.
        listing = "".join(p + "\0" for p in paths)
        stdin = pathlib.Path(d) / "files"
        stdin.write_text(listing, encoding="utf-8")
        # Swap only the `git diff ...` command for a `cat` of the fixture, leaving the
        # surrounding `< <( ... )` process substitution -- and every branch of the
        # classifier -- exactly as CI runs it.
        body, n = re.subn(r"git diff\b[^\n)]*", f"cat {stdin} ", BODY)
        if n != 1:
            sys.exit(f"expected exactly one git diff in the classifier, found {n}")
        script.write_text(body, encoding="utf-8")
        env = {"EVENT_NAME": event, "GITHUB_OUTPUT": str(out),
               "BASE_SHA": "a", "HEAD_SHA": "b", "PATH": "/usr/bin:/bin"}
        r = subprocess.run(["bash", str(script)], env=env, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"classifier failed: {r.stderr[-400:]}")
        kv = {}
        for line in out.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k] = v
        return kv


def expect(paths, want_only, want_changed, why):
    got = classify(paths)
    ok = got.get("docs_only") == want_only and got.get("docs_changed") == want_changed
    print(("  ok    " if ok else "  FAIL  ") +
          f"{why}: docs_only={got.get('docs_only')} docs_changed={got.get('docs_changed')}")
    if not ok:
        print(f"        expected docs_only={want_only} docs_changed={want_changed}")
    return ok


def main():
    fails = 0

    # Docs alone: skip the heavy gate, and check the docs.
    fails += not expect(["docs/SYNTHESIS_MODELS.md"], "true", "true", "docs only")
    fails += not expect(["README.md", "docs/images/x.svg"], "true", "true", "docs + image")

    # THE REGRESSION. Docs plus code: the heavy gate must run AND the docs must be
    # checked. Before docs_changed existed this returned docs_only=false and nothing
    # looked at the documentation.
    fails += not expect(["docs/SYNTHESIS_MODELS.md", "frontend/src/setup/deployTopology.ts"],
                        "false", "true", "docs + code (this is #2280)")
    fails += not expect(["docs/QUICKSTART.md", "src/kb/kb_service.c"],
                        "false", "true", "docs + C")

    # Code alone: full gate, no docs check to run.
    fails += not expect(["src/kb/kb_service.c"], "false", "false", "code only")
    fails += not expect(["Dockerfile"], "false", "false", "Dockerfile only")

    # A nested README under an allowlisted directory is documentation.
    fails += not expect(["src/README.md"], "true", "true", "nested allowlisted README")

    # Pushes and manual runs take the full gate and always check docs.
    got = classify(["anything"], event="push")
    ok = got.get("docs_only") == "false" and got.get("docs_changed") == "true"
    print(("  ok    " if ok else "  FAIL  ") +
          f"push: docs_only={got.get('docs_only')} docs_changed={got.get('docs_changed')}")
    fails += not ok

    if fails:
        sys.exit(f"test_ci_change_scope: {fails} failure(s)")
    print("test_ci_change_scope: ok")


if __name__ == "__main__":
    main()
