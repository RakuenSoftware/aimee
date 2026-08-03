#!/usr/bin/env python3
"""Wrapper for aimee's `memory_rewrite_command` contract.

Plumbs an OpenAI-compat endpoint (via `llm-chat.py`) into the query-rewrite
pipeline in `src/memory_core_search.inc:memory_query_rewrite`.  Contract:

  stdin  (JSON):
    {
      "query": "...",              # the user's query
      "hyde": true/false,          # ask for a HyDE hypothetical answer?
      "decompose": true/false,     # ask for sub-questions?
      "max_subqueries": N          # cap on sub-question count
    }

  stdout (JSON):
    {
      "hyde_answer": "...",        # plain-prose hypothetical answer, or ""
      "sub_questions": ["...", ...]# 0..max_subqueries decomposed queries
    }

Both output fields are optional.  On any error we emit `{}` on stdout and
exit 0 — the C caller treats a missing field as "not generated" and falls
back to the original query.  Exiting with an error would cause aimee to
log a rewrite failure for every query; silent fallback is the friendlier
default for a local-LLM setup where the endpoint might occasionally hiccup.

Configuration: all env vars honored by `scripts/llm-chat.py`
(SYNTHESIS_ENDPOINT, SYNTHESIS_MODEL, SYNTHESIS_API_KEY, LLM_NO_THINKING, ...).
Point aimee at this wrapper:

  aimee config set memory_rewrite_command "python3 $AIMEE_ROOT/scripts/llm-rewrite.py"
  aimee config set memory_rewrite_enabled 1
  aimee config set memory_rewrite_hyde 1
  aimee config set memory_rewrite_decompose 1

Stdlib only.  Requires `llm-chat.py` in the same directory.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


def _emit_empty() -> None:
    json.dump({}, sys.stdout)
    sys.stdout.write("\n")


def _build_prompt(query: str, want_hyde: bool, want_decompose: bool, max_sub: int) -> str:
    """Single prompt that asks for both HyDE and decomposition in one call.

    The model returns a JSON object — we parse and project back into the
    aimee contract.  Collapsing both asks into a single LLM turn halves
    the per-query rewrite cost."""
    tasks: list[str] = []
    if want_hyde:
        tasks.append(
            "- hyde_answer: a single short hypothetical answer to the query, "
            "as if you knew the fact.  1-3 sentences, declarative prose, no caveats."
        )
    if want_decompose:
        tasks.append(
            f"- sub_questions: up to {max_sub} simpler sub-queries that together "
            "cover the original.  Return an array of strings (may be empty if the "
            "original query is already atomic).  Strip 'how/why' framing when the "
            "sub-question is a plain factual lookup."
        )
    tasks_str = "\n".join(tasks) if tasks else "(no tasks — return {})"

    return (
        "You are a query-rewrite assistant for a retrieval system.  Given "
        "the user's query, produce the following fields:\n"
        f"{tasks_str}\n\n"
        "Respond with a single JSON object.  No prose, no code fences, no commentary.\n"
        "Schema:\n"
        '  {"hyde_answer": "..." (or ""), "sub_questions": [...] (or [])}\n\n'
        f"Query: {query}"
    )


def _call_llm(prompt: str) -> str | None:
    script_dir = Path(__file__).resolve().parent
    chat = script_dir / "llm-chat.py"
    if not chat.exists():
        print(f"llm-rewrite: sibling llm-chat.py not found at {chat}", file=sys.stderr)
        return None
    try:
        proc = subprocess.run(
            [sys.executable, str(chat), "--prompt", prompt],
            env=os.environ,
            capture_output=True,
            text=True,
            check=False,
            timeout=int(os.environ.get("LLM_TIMEOUT", "120")) + 5,
        )
    except subprocess.SubprocessError as exc:
        print(f"llm-rewrite: llm-chat subprocess failed: {exc}", file=sys.stderr)
        return None
    if proc.returncode != 0:
        print(
            f"llm-rewrite: llm-chat exit={proc.returncode}: "
            f"{(proc.stderr or proc.stdout)[:300]}",
            file=sys.stderr,
        )
        return None
    return proc.stdout


def _extract_json_object(text: str) -> dict | None:
    """LLMs sometimes wrap JSON in prose or fences even when asked not to.
    Grab the first {...} block we can successfully parse."""
    if not text:
        return None
    # Fast path: whole string is JSON.
    try:
        obj = json.loads(text)
        return obj if isinstance(obj, dict) else None
    except json.JSONDecodeError:
        pass
    start = text.find("{")
    while start != -1:
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    try:
                        obj = json.loads(text[start : i + 1])
                        if isinstance(obj, dict):
                            return obj
                    except json.JSONDecodeError:
                        pass
                    break
        start = text.find("{", start + 1)
    return None


def main() -> None:
    try:
        raw_input = sys.stdin.read()
    except OSError:
        _emit_empty()
        return
    if not raw_input.strip():
        _emit_empty()
        return
    try:
        req = json.loads(raw_input)
    except json.JSONDecodeError:
        print("llm-rewrite: input is not valid JSON", file=sys.stderr)
        _emit_empty()
        return

    query = str(req.get("query", "")).strip()
    if not query:
        _emit_empty()
        return
    want_hyde = bool(req.get("hyde", False))
    want_decompose = bool(req.get("decompose", False))
    max_sub = int(req.get("max_subqueries", 4))
    if max_sub < 0:
        max_sub = 0

    if not (want_hyde or want_decompose):
        _emit_empty()
        return

    prompt = _build_prompt(query, want_hyde, want_decompose, max_sub)
    raw_response = _call_llm(prompt)
    if raw_response is None:
        _emit_empty()
        return

    parsed = _extract_json_object(raw_response)
    if parsed is None:
        print("llm-rewrite: could not extract JSON from LLM response", file=sys.stderr)
        _emit_empty()
        return

    out: dict = {}
    if want_hyde:
        ha = parsed.get("hyde_answer")
        if isinstance(ha, str) and ha.strip():
            out["hyde_answer"] = ha.strip()
    if want_decompose:
        sq = parsed.get("sub_questions")
        if isinstance(sq, list):
            cleaned = [str(s).strip() for s in sq if isinstance(s, (str, int, float)) and str(s).strip()]
            if cleaned:
                out["sub_questions"] = cleaned[:max_sub]

    json.dump(out, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
