#!/usr/bin/env python3
"""curator-synthesize.py: deep-curator synthesize_topic sidecar.

Reads a synthesis request on stdin and writes a synthesis result on stdout, so
the curator drain's synthesize_topic pass can call any OpenAI-compatible LLM
(the reference companion to curator-extract.py for extraction). Point
`kb.curator.synthesize_command` at `python3 .../curator-synthesize.py`.

Request (stdin):
    {"task": "synthesize_topic",
     "topic": {"id": "...", "name": "..."},
     "sources": [{"id": "...", "kind": "claim|doc_summary|entity|...",
                  "payload": {...}}, ...]}

Response (stdout):
    {"version": 1, "status": "ok", "synthesis": "<prose synthesis>"}
  or {"version": 1, "status": "error", "error": "..."}

Env: LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY, LLM_MAX_TOKENS, LLM_TIMEOUT (same as
llm-chat.py). CURATOR_LLM_STUB_FILE short-circuits the model with file contents
for deterministic CI. Exit codes: 0 on success, 1 on any error.
"""

import json
import os
import subprocess
import sys


def emit_error(msg: str) -> None:
    json.dump({"version": 1, "status": "error", "error": msg}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    sys.exit(1)


def strip_think(text: str) -> str:
    """Drop a reasoning model's <think>...</think> preamble and code fences."""
    text = text.strip()
    # Only a block at the very START is a reasoning preamble; an occurrence
    # later belongs to the content. Splitting on the last tag anywhere silently
    # destroyed answers that merely mentioned it (see curator-extract.py).
    if text.startswith("<think>"):
        end = text.find("</think>")
        if end != -1:
            text = text[end + len("</think>") :].strip()
    if text.startswith("```"):
        lines = text.splitlines()
        end = len(lines) - 1 if lines and lines[-1].strip() == "```" else len(lines)
        text = "\n".join(lines[1:end]).strip()
    return text.strip()


def build_prompt(topic_name: str, sources: list) -> str:
    """Compose a synthesis prompt from the topic and its cited sources."""
    lines = [
        "You are a knowledge curator. Write a single coherent synthesis of what "
        f'is known about the topic "{topic_name}", grounded ONLY in the sources '
        "below. Integrate agreements, note tensions, and stay concise (one to "
        "three short paragraphs). Output the synthesis prose only — no preamble, "
        "no headings, no JSON.",
        "",
        "SOURCES:",
    ]
    for i, src in enumerate(sources, 1):
        payload = src.get("payload", {})
        if isinstance(payload, str):
            try:
                payload = json.loads(payload)
            except Exception:
                payload = {"text": payload}
        kind = src.get("kind", "?")
        # Prefer the human-readable fields a curator artifact carries.
        body = (
            payload.get("text")
            or payload.get("summary")
            or payload.get("name")
            or json.dumps(payload, ensure_ascii=False)
        )
        lines.append(f"[{i}] ({kind}) {body}")
    return "\n".join(lines)


def call_llm(prompt: str, max_tokens: int) -> tuple:
    stub_path = os.environ.get("CURATOR_LLM_STUB_FILE", "")
    if stub_path:
        try:
            with open(stub_path, "r", encoding="utf-8") as fh:
                return fh.read().strip(), None
        except OSError as exc:
            return None, f"CURATOR_LLM_STUB_FILE unreadable: {exc}"

    script_dir = os.path.dirname(os.path.abspath(__file__))
    llm_script = os.path.join(script_dir, "llm-chat.py")

    env = os.environ.copy()
    env["LLM_NO_THINKING"] = "1"
    env["LLM_MAX_TOKENS"] = str(max_tokens)
    env.setdefault("LLM_ENDPOINT", "http://192.168.1.122:8080")
    env.setdefault("LLM_MODEL", "qwen3")

    timeout_s = int(env.get("LLM_TIMEOUT", "120"))
    try:
        result = subprocess.run(
            ["python3", llm_script],
            input=prompt,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            env=env,
        )
        if result.returncode != 0:
            return None, f"llm-chat.py exit {result.returncode}: {result.stderr[:300]}"
        return result.stdout.strip(), None
    except subprocess.TimeoutExpired:
        return None, f"llm-chat.py timed out after {timeout_s}s"
    except Exception as exc:
        return None, str(exc)


def main() -> None:
    try:
        req = json.load(sys.stdin)
    except Exception as exc:
        emit_error(f"invalid request JSON: {exc}")

    topic = req.get("topic", {})
    topic_name = topic.get("name", "") if isinstance(topic, dict) else ""
    sources = req.get("sources", [])
    if not isinstance(sources, list) or not sources:
        emit_error("no sources to synthesize from")

    max_tokens = int(req.get("config", {}).get("max_tokens", 1024) if isinstance(
        req.get("config"), dict) else 1024)

    prompt = build_prompt(topic_name, sources)
    raw, err = call_llm(prompt, max_tokens)
    if err:
        emit_error(err)

    synthesis = strip_think(raw or "")
    if not synthesis:
        emit_error("LLM returned empty synthesis")

    json.dump({"version": 1, "status": "ok", "synthesis": synthesis}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
