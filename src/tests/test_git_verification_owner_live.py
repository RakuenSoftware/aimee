#!/usr/bin/env python3
"""Real daemon verification must distinguish clean source from unknown state.

This needs no Docker and grants no independent test grade. It catches a missing
Git resource owner before the separate private-Docker journey can skip locally.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

from test_native_ir_live import api


def main():
    # Registration alone is insufficient: diagnose selects current_code, which
    # intentionally does not inherit core/index tools. Catch a hidden verifier
    # before Docker-dependent child execution can skip locally.
    resolved = api("/v1/toolsets/resolve", dict(name="current_code"))
    assert resolved.get("status") == "ok" and "git_verify" in resolved.get("tools", []), resolved
    assert not set(resolved["tools"]) & {"git_commit", "git_push", "git_branch", "git_pr",
        "index_ingest", "kb_build", "memory_store", "find_symbol", "search_memory"}, resolved
    home = Path(os.environ["AIMEE_HOME"])
    with tempfile.TemporaryDirectory(prefix="git-owner-", dir=home.parent) as temporary:
        root = Path(temporary)
        for arguments in (("init", "-q", "-b", "main"),
                          ("config", "user.name", "Integration Fixture"),
                          ("config", "user.email", "fixture@example.invalid")):
            subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True)
        (root / ".gitignore").write_text(".aimee/\n")
        (root / "receipt.txt").write_text("SOURCE_OWNER_READY\n")
        subprocess.run(["git", "-C", str(root), "add", ".gitignore", "receipt.txt"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "verification owner fixture"], check=True)
        registration = api("/v1/workspaces", dict(root=str(root), provider="shared", scan=False))
        assert registration.get("status") == "ok", registration
        configuration = home / "projects" / root.name / "project.yaml"
        configuration.parent.mkdir(parents=True)
        configuration.write_text("verify:\n  incremental: false\n  enforce: true\n  steps:\n"
                                 "    - name: source-owner-ready\n"
                                 '      run: test "$(cat receipt.txt)" = SOURCE_OWNER_READY\n')
        session = api("/v1/sessions/create", dict(client_type="git-owner-journey"))["session_id"]
        try:
            response = api("/v1/mcp/call", dict(tool="git_verify", session_id=session,
                arguments={"path": str(root), "no_session_redirect": True, "action": "run",
                           "async": False, "format": "json"}))
            assert isinstance(response, dict), response
            content = response.get("content", [])
            verdicts = [json.loads(item["text"]) for item in content if item.get("type") == "text"]
            assert len(verdicts) == 1, response
            verdict = verdicts[0]
            assert verdict.get("verdict") == "passed" and verdict.get("total") == 1, verdict
            assert verdict.get("has_uncommitted_changes") is False, verdict
            # Actual worktree state is observed afresh, not cached as clean.
            (root / "dirty.txt").write_text("untracked source\n")
            response = api("/v1/mcp/call", dict(tool="git_verify", session_id=session,
                arguments={"path": str(root), "no_session_redirect": True, "action": "run",
                           "async": False, "format": "json"}))
            verdict = json.loads(response["content"][0]["text"])
            assert verdict.get("verdict") == "passed" and verdict.get("has_uncommitted_changes") is True, verdict
        finally:
            configuration.unlink(missing_ok=True)
            api("/v1/sessions/close", dict(session_id=session))
    print("Git verification owner: actual daemon clean/dirty source measurements and configured command pass without Docker")


if __name__ == "__main__":
    main()
