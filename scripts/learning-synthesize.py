#!/usr/bin/env python3
"""learning-synthesize.py: cross-source learning candidate-generation sidecar.

Reads a JSON request describing an evidence neighbourhood from stdin, asks the
local LLM (via llm-chat.py) to synthesise consolidated learning candidates that
the evidence jointly supports, and writes a JSON candidate list to stdout.

Request shape (stdin):
  {
    "role": "synthesize",
    "input": {
      "query": "the seed text the neighbourhood was built from",
      "neighbours": [
        {"artifact_id": "...", "kind": "feedback_negative",
         "content": "...", "score": 0.91},
        ...
      ]
    },
    "config": {"max_tokens": 2048}
  }

Response shape (stdout):
  {
    "version": 1,
    "status": "ok",
    "candidates": [
      {"kind": "anti_pattern", "payload": {...}, "confidence": 0.82},
      {"kind": "synthesis",    "payload": {...}, "confidence": 0.75}
    ]
  }

The C worker (kb_learning_synth.c) cites every neighbourhood artifact as a
source on each emitted candidate, so the judge's corroboration count is the
neighbourhood size — the sidecar only decides *what* to propose, never how it
is committed.

Exit codes: 0 on success, 1 on any error.
"""

import json
import os
import subprocess
import sys
import textwrap

# Candidate kinds the downstream promotion machinery understands. Anything else
# the model emits is dropped by the C worker, but we constrain the prompt too.
ALLOWED_KINDS = [
    "preference",
    "workflow",
    "anti_pattern",
    "mistake_pattern",
    "synthesis",
]


def resolve_llm_endpoint(env) -> tuple[str, str | None]:
    """OpenAI-compatible base URL for the sidecar, or an error to report.

    SYNTHESIS_ENDPOINT is the one endpoint the kb is configured with, and it is the
    BASE url — this appends /v1 itself. It used to read LLM_ENDPOINT first and fall
    back to AIMEE_LLM_URL: two spellings of one endpoint, so the sidecar and the kb
    could be pointed at different hosts, and setting only one split them silently.
    The rest of this note describes the one
    endpoint the kb is actually configured with (the container sets it, and the
    Dockerfile's own comment says "endpoints come from env").

    There is deliberately NO baked-in default. This used to fall back to a
    hardcoded LAN address, which failed two ways: where nothing owned that IP,
    every curator code-extraction job failed `No route to host` and the queue
    never drained; and where something DID own it, the sidecar posted the
    user's code and prompts to a machine that just happened to hold that
    address. embed-remote.py already removed its equivalent fallback for the
    same reason. Fail closed instead.
    """
    base = env.get("SYNTHESIS_ENDPOINT", "").strip().rstrip("/")
    if not base:
        return "", "no synthesis endpoint configured: set SYNTHESIS_ENDPOINT"
    return (base if base.endswith("/v1") else base + "/v1"), None


def emit_error(msg: str) -> None:
    json.dump({"version": 1, "status": "error", "error": msg}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    sys.exit(1)


def build_prompt(inp: dict) -> str:
    query = inp.get("query", "")
    neighbours = inp.get("neighbours", []) or []

    lines = []
    for i, n in enumerate(neighbours):
        kind = n.get("kind", "evidence")
        score = n.get("score", 0.0)
        content = (n.get("content", "") or "").strip().replace("\n", " ")
        if len(content) > 600:
            content = content[:600] + "…"
        lines.append(f"  [{i + 1}] ({kind}, sim={score:.2f}) {content}")
    neighbourhood = "\n".join(lines) if lines else "  (none)"

    kinds = ", ".join(ALLOWED_KINDS)
    return textwrap.dedent(f"""
        You are a learning-synthesis assistant. Below is a neighbourhood of
        related evidence drawn from MULTIPLE sources (feedback, guardrail events,
        session turns, run outcomes). They were retrieved because they are
        semantically close to this seed:

            {query}

        Evidence neighbourhood:
        {neighbourhood}

        Synthesise the durable lesson(s) this evidence JOINTLY supports — a
        consolidated learning the system should remember. Prefer a single strong
        candidate over many weak ones. Only propose a candidate when at least two
        of the evidence items corroborate it. Return ONLY a valid JSON object —
        no markdown fences — of this exact shape:

        {{
          "candidates": [
            {{
              "kind": "<one of: {kinds}>",
              "payload": {{
                "title": "short imperative summary",
                "content": "one-paragraph statement of the lesson",
                "rationale": "why the evidence supports it"
              }},
              "confidence": 0.80
            }}
          ]
        }}

        If the evidence does not jointly support any durable lesson, return
        {{"candidates": []}}.
    """).strip()


def call_llm(prompt: str, max_tokens: int) -> tuple[str | None, str | None]:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    llm_script = os.path.join(script_dir, "llm-chat.py")

    env = os.environ.copy()
    env["LLM_NO_THINKING"] = "1"
    env["LLM_MAX_TOKENS"] = str(max_tokens)
    endpoint, endpoint_err = resolve_llm_endpoint(env)
    if endpoint_err:
        return None, endpoint_err
    env["SYNTHESIS_ENDPOINT"] = endpoint
    env.setdefault("SYNTHESIS_MODEL", "qwen3")

    try:
        result = subprocess.run(
            ["python3", llm_script],
            input=prompt,
            capture_output=True,
            text=True,
            timeout=120,
            env=env,
        )
        if result.returncode != 0:
            return None, f"llm-chat.py exit {result.returncode}: {result.stderr[:300]}"
        return result.stdout.strip(), None
    except subprocess.TimeoutExpired:
        return None, "llm-chat.py timed out after 120s"
    except Exception as exc:  # noqa: BLE001
        return None, str(exc)


def strip_fences(text: str) -> str:
    text = text.strip()
    if text.startswith("```"):
        lines = text.splitlines()
        end = len(lines) - 1 if lines[-1].strip() == "```" else len(lines)
        text = "\n".join(lines[1:end])
    return text.strip()


def main() -> None:
    try:
        req = json.load(sys.stdin)
    except Exception as exc:  # noqa: BLE001
        emit_error(f"invalid JSON input: {exc}")

    role = req.get("role", "")
    if role != "synthesize":
        emit_error(f"unknown role: {role!r}")

    inp = req.get("input", {}) or {}
    max_tokens = req.get("config", {}).get("max_tokens", 2048)

    prompt = build_prompt(inp)
    raw, llm_err = call_llm(prompt, max_tokens)
    if llm_err:
        emit_error(llm_err)

    text = strip_fences(raw or "")
    try:
        parsed = json.loads(text)
    except Exception as exc:  # noqa: BLE001
        emit_error(f"LLM returned non-JSON: {exc}: {text[:300]}")

    raw_candidates = parsed.get("candidates", [])
    if not isinstance(raw_candidates, list):
        emit_error("LLM 'candidates' field is not a list")

    candidates = []
    for c in raw_candidates:
        if not isinstance(c, dict):
            continue
        kind = c.get("kind", "")
        if kind not in ALLOWED_KINDS:
            continue
        payload = c.get("payload", {})
        if not isinstance(payload, dict):
            continue
        try:
            confidence = float(c.get("confidence", 0.0))
        except (TypeError, ValueError):
            confidence = 0.0
        confidence = max(0.0, min(1.0, confidence))
        candidates.append({"kind": kind, "payload": payload, "confidence": confidence})

    json.dump({"version": 1, "status": "ok", "candidates": candidates}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
