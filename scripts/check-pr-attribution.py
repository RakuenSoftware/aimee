#!/usr/bin/env python3
"""Reject AI attribution in live GitHub issue metadata for a pull request.

The opening PR entry's `with <app>` badge comes from performed_via_github_app,
not its Markdown body. Fetch the issue representation: the pulls endpoint and
webhook payload need not include that field. Missing metadata is not a pass.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

AI_APP_IDS = {1236702, 1144995}  # Claude; ChatGPT Codex Connector
AI_APP_SLUGS = {
    "claude", "claude-code", "chatgpt-codex-connector", "codex",
    "copilot", "copilot-swe-agent", "github-copilot",
}
AI_APP_NAMES = {"claude", "claude code", "chatgpt codex connector", "codex", "copilot", "github copilot"}
ATTRIBUTION = re.compile(
    r"^\s*(?:co-authored-by:|(?:claude|codex|chatgpt|copilot)-session:|"
    r"https?://\S*/(?:code/)?session[_/-]|"
    r"[_*]*generated\s+(?:with|by)\s+\[(?:claude|codex|chatgpt|copilot|github copilot)\b)",
    re.IGNORECASE | re.MULTILINE,
)


def violations(issue: object) -> list[str]:
    if not isinstance(issue, dict) or not isinstance(issue.get("number"), int):
        raise ValueError("expected a GitHub issue representation for the PR")
    if "performed_via_github_app" not in issue:
        raise ValueError("missing performed_via_github_app; fetch /repos/{owner}/{repo}/issues/{number}")
    if not isinstance(issue.get("title"), str) or not (issue.get("body") is None or isinstance(issue.get("body"), str)):
        raise ValueError("invalid PR title/body metadata")
    problems = []
    app = issue["performed_via_github_app"]
    if app is not None:
        if not isinstance(app, dict) or type(app.get("id")) is not int or not isinstance(app.get("slug"), str) or not app["slug"]:
            raise ValueError("invalid performed_via_github_app metadata")
        if (app["id"] in AI_APP_IDS or app["slug"].casefold() in AI_APP_SLUGS
                or str(app.get("name", "")).casefold() in AI_APP_NAMES):
            problems.append(f"PR #{issue['number']} carries GitHub app attribution: {app['slug']} (performed_via_github_app)")
    for field in ("title", "body"):
        if ATTRIBUTION.search(issue.get(field) or ""):
            problems.append(f"PR #{issue['number']} {field} carries AI attribution")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("issue_json", type=Path)
    args = parser.parse_args()
    try:
        problems = violations(json.loads(args.issue_json.read_text(encoding="utf-8")))
    except (OSError, ValueError) as error:
        print(f"::error::Cannot verify PR attribution: {error}", file=sys.stderr)
        return 1
    if problems:
        for problem in problems:
            print(f"::error::{problem}", file=sys.stderr)
        return 1
    print("no AI attribution in PR title/body or GitHub app metadata — ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
