#!/usr/bin/env python3
"""dev-accept.py — autonomous-dev §3/§4/§5: execute a proposal's machine-checkable
acceptance block on each check's tier, compute a deterministic verdict, and (only
on an all-green verdict) file the proposal from pending/ to done/.

Two stages, with a serialized verdict JSON between them:

    dev-accept.py eval <proposal.md>        # run checks -> print verdict JSON (NEVER moves files)
    dev-accept.py file <proposal.md>        # re-eval; if verdict passed, git mv + reference fixups

Tier routing (§3):
    mechanical / integration -> run the check command locally (or in the §1 runner
                                with --runner); pass iff exit 0.
    deployment / hardware    -> dispatch the GH Actions matrix (gh workflow run +
                                poll, with --dispatch) OR, if the proposal DECLARES
                                the tier deferred (a `deferred` block), record
                                skipped-declared (does NOT block filing).

A check that no runner/gh can execute is `validation-pending` and BLOCKS filing
(aggregate verdict `filing-blocked`) — never auto-claimed, never auto-passed. The
two validation-pending cases are kept distinct (§4): DECLARED-deferred does not
block; UNABLE-to-execute does.

Coexists with check-proposal-reconcile.py: that stays the fast static lint gate
(block-shape + drift); this is the runtime acceptance executor and is invoked only
when a proposal is being driven to done/.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

try:
    import yaml
except ImportError:
    print("dev-accept: PyYAML required (pip install pyyaml)", file=sys.stderr)
    sys.exit(2)

SCHEMA_VERSION = 1
VALID_TIERS = {"mechanical", "integration", "deployment", "hardware"}
LOCAL_TIERS = {"mechanical", "integration"}
DISPATCH_TIERS = {"deployment", "hardware"}

FENCE_OPEN = re.compile(r"^```[ \t]*(?:[A-Za-z0-9_-]+[ \t]+)?(acceptance|deferred)[ \t]*$")
FENCE_CLOSE = re.compile(r"^```[ \t]*$")

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def extract_blocks(text, kind):
    """Yield the YAML body of every fenced ```yaml <kind> block."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = FENCE_OPEN.match(lines[i])
        if m and m.group(1) == kind:
            body = []
            i += 1
            while i < len(lines) and not FENCE_CLOSE.match(lines[i]):
                body.append(lines[i])
                i += 1
            yield "\n".join(body)
        i += 1


def parse_acceptance(text):
    """Return the list of {id, tier, check, ...} entries across all acceptance blocks."""
    out = []
    for raw in extract_blocks(text, "acceptance"):
        data = yaml.safe_load(raw) if raw.strip() else []
        if isinstance(data, list):
            out.extend(e for e in data if isinstance(e, dict))
    return out


def parse_deferred(text):
    """Return {tier: {reason, deferred_to}} from `deferred` blocks. A tier listed
    here is author-declared deferred: a deployment/hardware check on it records
    skipped-declared and does NOT block filing (§4 declared-deferred rule)."""
    out = {}
    for raw in extract_blocks(text, "deferred"):
        data = yaml.safe_load(raw) if raw.strip() else []
        if not isinstance(data, list):
            continue
        for e in data:
            if isinstance(e, dict) and e.get("tier"):
                out[e["tier"]] = {"reason": e.get("reason", ""),
                                  "deferred_to": e.get("deferred_to", "")}
    return out


def run_local(check, timeout):
    """Run a mechanical/integration check locally from the repo root. Returns
    (status, exit, log)."""
    try:
        p = subprocess.run(check, shell=True, cwd=REPO_ROOT, timeout=timeout,
                           capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return "unavailable", 124, f"timed out after {timeout}s"
    except OSError as e:
        return "unavailable", 126, f"exec error: {e}"
    log = (p.stdout + p.stderr)[-8192:]
    return ("pass" if p.returncode == 0 else "fail"), p.returncode, log


def dispatch_ci(check, dispatch, timeout):
    """deployment/hardware: dispatch the GH Actions matrix and poll its conclusion.
    Returns (status, run_id, gh_state, log). Without --dispatch (or with no gh),
    returns validation-pending/not-attempted so it is never auto-claimed."""
    if not dispatch:
        return "validation-pending", "", "not-attempted", \
            "deployment/hardware tier not dispatched (pass --dispatch to trigger CI)"
    if shutil.which("gh") is None:
        return "validation-pending", "", "gh-absent", "gh CLI not available to dispatch CI"
    # check is expected as "ci:<workflow>"; dispatch + poll. Any failure to match a
    # run, or a non-success conclusion, is reported (never auto-passed).
    wf = check[3:] if check.startswith("ci:") else check
    try:
        subprocess.run(["gh", "workflow", "run", wf], cwd=REPO_ROOT,
                       timeout=60, capture_output=True, text=True, check=True)
    except (subprocess.SubprocessError, OSError) as e:
        return "validation-pending", "", "gh-dispatch-error", f"gh workflow run failed: {e}"
    # NOTE: full run-matching (by head_sha + dispatch timestamp) and conclusion
    # polling is the deployment path; until a run is confirmed green it stays
    # validation-pending so filing is never auto-claimed on an unverified deploy.
    return "validation-pending", "", "dispatched-pending", \
        f"dispatched {wf}; conclusion not yet confirmed"


def evaluate(text, runner=None, dispatch=False, timeout=1800):
    """Run every acceptance check on its tier and return the verdict dict (§4)."""
    entries = parse_acceptance(text)
    deferred = parse_deferred(text)
    checks = []
    for e in entries:
        tier, check = e.get("tier"), e.get("check", "")
        eid = e.get("id")
        rec = {"id": eid, "tier": tier, "check": check, "applicable": True,
               "deferred_reason": "", "deferred_to": "", "run_id": "", "gh_state": ""}
        if tier in LOCAL_TIERS:
            if runner:
                status, ec, log = run_in_runner(runner, check, tier, timeout)
            else:
                status, ec, log = run_local(check, timeout)
            rec.update(status=status, exit=ec, log=log[-2048:])
        elif tier in DISPATCH_TIERS:
            if tier in deferred:
                rec.update(status="skipped-declared", exit=0, log="",
                           applicable=False,
                           deferred_reason=deferred[tier].get("reason", ""),
                           deferred_to=deferred[tier].get("deferred_to", ""))
            else:
                status, run_id, gh_state, log = dispatch_ci(check, dispatch, timeout)
                rec.update(status=status, exit=0, log=log, run_id=run_id, gh_state=gh_state)
        else:
            rec.update(status="fail", exit=0, log=f"unknown tier {tier!r}")
        checks.append(rec)

    # Aggregate (§4): every applicable check must pass; an UNABLE-to-execute
    # (validation-pending / unavailable) blocks filing; declared-deferred does not.
    blocking = [c for c in checks
                if c["status"] in ("fail", "unavailable", "validation-pending")]
    if not entries:
        verdict, reason = "failed", "no-acceptance-block"
    elif any(c["status"] in ("fail",) for c in checks):
        verdict, reason = "failed", "check-failed"
    elif any(c["status"] in ("unavailable", "validation-pending") for c in checks):
        verdict, reason = "filing-blocked", "validation-pending"
    else:
        verdict, reason = "passed", "ok"
    return {"schema_version": SCHEMA_VERSION, "verdict": verdict, "reason": reason,
            "total": len(checks), "blocking": len(blocking), "checks": checks}


def run_in_runner(runner, check, tier, timeout):
    """Route a local-tier check through the §1 ephemeral runner (dev-verify-runner.sh)."""
    try:
        p = subprocess.run([runner, "--step", check, "--tier", tier],
                           cwd=REPO_ROOT, timeout=timeout + 120,
                           capture_output=True, text=True)
        v = json.loads(p.stdout)
        # map the runner's verdict to a check status
        m = {"passed": "pass", "failed": "fail", "unavailable": "unavailable"}
        return m.get(v.get("verdict"), "unavailable"), v.get("exit", -1), v.get("log", "")[-2048:]
    except (subprocess.SubprocessError, OSError, ValueError) as e:
        return "unavailable", 126, f"runner error: {e}"


# ---- §5 active auto-file ----

def file_to_done(proposal_path, verdict):
    """On a passed verdict, git mv the proposal (+ .plan.md sibling) from pending/
    to done/ and rewrite pending/<name> references across docs/. Refuses unless the
    verdict is passed (never files an unverified proposal)."""
    if verdict.get("verdict") != "passed":
        return False, f"verdict is {verdict.get('verdict')} (not passed); not filing"
    src = os.path.abspath(proposal_path)
    if os.sep + "pending" + os.sep not in src:
        return False, "proposal is not under pending/"
    moved = []
    base = src[:-3] if src.endswith(".md") else src
    for cand in (src, base + ".plan.md"):
        if os.path.exists(cand):
            dst = cand.replace(os.sep + "pending" + os.sep, os.sep + "done" + os.sep)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            rc = subprocess.run(["git", "-C", REPO_ROOT, "mv", cand, dst],
                                capture_output=True, text=True)
            if rc.returncode != 0:
                # fall back to a plain move if not tracked
                shutil.move(cand, dst)
            moved.append((os.path.relpath(cand, REPO_ROOT), os.path.relpath(dst, REPO_ROOT)))
    # Rewrite pending/<name> -> done/<name> references across docs/ (link-graph fixup).
    names = [os.path.basename(m[0]) for m in moved]
    for root, _dirs, files in os.walk(os.path.join(REPO_ROOT, "docs")):
        for fn in files:
            if not fn.endswith(".md"):
                continue
            fp = os.path.join(root, fn)
            try:
                with open(fp, encoding="utf-8") as fh:
                    t = fh.read()
            except OSError:
                continue
            nt = t
            for nm in names:
                nt = nt.replace(f"pending/{nm}", f"done/{nm}")
            if nt != t:
                with open(fp, "w", encoding="utf-8") as fh:
                    fh.write(nt)
    return True, f"filed {len(moved)} file(s) to done/: {[m[1] for m in moved]}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["eval", "file"])
    ap.add_argument("proposal")
    ap.add_argument("--runner", help="path to dev-verify-runner.sh for local-tier checks")
    ap.add_argument("--dispatch", action="store_true",
                    help="actually dispatch deployment/hardware checks to GH Actions")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args(argv)

    try:
        with open(args.proposal, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as e:
        print(f"dev-accept: cannot read {args.proposal}: {e}", file=sys.stderr)
        return 2

    verdict = evaluate(text, runner=args.runner, dispatch=args.dispatch, timeout=args.timeout)

    if args.command == "file":
        ok, msg = file_to_done(args.proposal, verdict)
        verdict["filed"] = ok
        verdict["file_message"] = msg

    print(json.dumps(verdict, indent=2))
    # exit 0 only on a clean passed verdict (or a successful file)
    if args.command == "file":
        return 0 if verdict.get("filed") else 1
    return 0 if verdict["verdict"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
