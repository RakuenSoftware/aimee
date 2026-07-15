#!/usr/bin/env python3
"""Generic OpenAI-compat chat client — stdin (or --prompt) in, content out.

Designed as a sidecar for every aimee config that expects a CLI to pipe
through (e.g. `memory_rewrite_command`, wrapper scripts in `scripts/`).
Works with any endpoint that speaks OpenAI's /v1/chat/completions:

  - OpenAI itself (api.openai.com/v1)
  - Local llama.cpp / llama-server
  - Ollama (http://host:11434/v1)
  - Together, Groq, Anthropic-via-openai-proxy, vLLM, LM Studio, ...

All configuration flows through env vars (suitable for setting once in
aimee's config) with CLI-flag overrides for one-off use.

  LLM_ENDPOINT     base URL, e.g. http://192.168.0.115:8080/v1
  LLM_MODEL        model id, e.g. qwen3.6 or gpt-4o-mini
  LLM_API_KEY      bearer token; if it starts with "cmd:", the remainder
                   is executed as a shell command whose stdout is the key
  LLM_SYSTEM       default system prompt (optional)
  LLM_TEMPERATURE  default 0.0
  LLM_MAX_TOKENS   default 2048
  LLM_NO_THINKING  "1" to pass chat_template_kwargs.enable_thinking=false
                   (Qwen-family hybrid-thinking models)
  LLM_TIMEOUT      seconds, default 120
  LLM_RETRIES      default 2 (exponential backoff on 5xx / connection errors)

Stdlib only.  Runs on any Python 3.9+.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any


def _resolve_api_key(raw: str) -> str:
    if not raw:
        return ""
    if raw.startswith("cmd:"):
        cmd = raw[4:].strip()
        try:
            out = subprocess.check_output(cmd, shell=True, text=True, timeout=15)
        except subprocess.SubprocessError as exc:
            sys.exit(f"llm-chat: api-key command failed: {exc}")
        return out.strip()
    return raw


def _read_prompt(cli_prompt: str | None, positional: str | None) -> str:
    if cli_prompt is not None:
        return cli_prompt
    if positional is not None:
        return positional
    if sys.stdin.isatty():
        sys.exit("llm-chat: no prompt (pass --prompt, a positional arg, or pipe stdin)")
    return sys.stdin.read()


def _build_body(args: argparse.Namespace, prompt: str) -> dict[str, Any]:
    messages: list[dict[str, Any]] = []
    if args.system:
        messages.append({"role": "system", "content": args.system})
    messages.append({"role": "user", "content": prompt})

    body: dict[str, Any] = {
        "model": args.model,
        "messages": messages,
        "temperature": args.temperature,
        "max_tokens": args.max_tokens,
    }
    if args.stream:
        body["stream"] = True
    if args.no_thinking:
        # Qwen-family hybrid-thinking models use this toggle; harmless on
        # endpoints that ignore unknown fields (OpenAI, Anthropic-proxy, ...).
        body["chat_template_kwargs"] = {"enable_thinking": False}
    if os.environ.get("LLM_JSON_MODE"):
        # OpenAI-compatible JSON mode: forces a well-formed JSON object body
        # (harmless on endpoints that ignore unknown fields).
        body["response_format"] = {"type": "json_object"}
    return body


def _post(url: str, body: dict[str, Any], api_key: str, timeout: int) -> urllib.request.addinfourl:
    headers = {"content-type": "application/json"}
    if api_key:
        headers["authorization"] = f"Bearer {api_key}"
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    return urllib.request.urlopen(req, timeout=timeout)


def _call_with_retries(args: argparse.Namespace, prompt: str) -> urllib.request.addinfourl:
    api_key = _resolve_api_key(args.api_key or "")
    url = f"{args.endpoint.rstrip('/')}/chat/completions"
    body = _build_body(args, prompt)

    last_exc: Exception | None = None
    for attempt in range(args.retries + 1):
        try:
            return _post(url, body, api_key, args.timeout)
        except urllib.error.HTTPError as exc:
            # 4xx is usually a client bug, don't retry.  5xx is worth a retry.
            if exc.code < 500 or attempt == args.retries:
                body_preview = exc.read()[:500] if hasattr(exc, "read") else b""
                sys.exit(
                    f"llm-chat: HTTP {exc.code} from {url}\n{body_preview.decode(errors='replace')}"
                )
            last_exc = exc
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            if attempt == args.retries:
                sys.exit(f"llm-chat: request to {url} failed after {attempt + 1} tries: {exc}")
            last_exc = exc
        time.sleep(min(2 ** attempt, 8))
    # Unreachable, but keeps mypy happy.
    raise RuntimeError(f"llm-chat: exhausted retries: {last_exc}")


def _stream_content(resp: urllib.request.addinfourl) -> None:
    """Print content chunks as they arrive; OpenAI-style SSE 'data: {...}\\n'."""
    for raw_line in resp:
        line = raw_line.decode("utf-8", errors="replace").strip()
        if not line or not line.startswith("data:"):
            continue
        payload = line[len("data:") :].strip()
        if payload == "[DONE]":
            break
        try:
            evt = json.loads(payload)
        except json.JSONDecodeError:
            continue
        delta = evt.get("choices", [{}])[0].get("delta", {}).get("content", "")
        if delta:
            sys.stdout.write(delta)
            sys.stdout.flush()
    # Ensure trailing newline for pipeline consumers.
    sys.stdout.write("\n")


def split_reasoning(message: dict[str, Any]) -> tuple[str, str]:
    """Return (content, reasoning) with reasoning separated from the answer.

    THE CONTRACT: stdout carries the answer and nothing else. Reasoning is a
    distinct thing and belongs in a distinct field — the same split
    aimee_ir.h models as AIMEE_BLK_THINKING.

    Compliant servers do this for us: llama.cpp under --jinja (and the DeepSeek/
    Qwen APIs) return a separate `reasoning_content`, leaving `content` clean.
    Endpoints that instead INLINE a <think>...</think> preamble get normalised to
    the same shape here — once, at the wire boundary, where the raw response is
    still structured — instead of every consumer re-deriving it from text.

    That re-derivation is exactly what went wrong: curator-extract.py split on
    the LAST "</think>" anywhere in the answer, so summarising a function whose
    own job is stripping think blocks cut the JSON mid-string and killed the job.
    Meanwhile llm-rewrite.py and learning-synthesize.py did no stripping at all —
    the same contract, understood three different ways. One place, one rule.
    """
    content = message.get("content") or ""
    reasoning = message.get("reasoning_content") or ""
    if reasoning:
        return content, reasoning          # server already split it; trust that
    # Inlining endpoint: a reasoning block is a PREFIX. An occurrence anywhere
    # else belongs to the answer (a docstring may legitimately discuss the tag).
    if content.lstrip().startswith("<think>"):
        head = content.lstrip()
        end = head.find("</think>")
        if end != -1:
            return head[end + len("</think>") :].strip(), head[len("<think>") : end].strip()
    return content, reasoning


def _emit(resp: urllib.request.addinfourl, emit_json: bool) -> None:
    payload = json.loads(resp.read())
    if emit_json:
        json.dump(payload, sys.stdout)
        sys.stdout.write("\n")
        return
    try:
        message = payload["choices"][0]["message"]
    except (KeyError, IndexError) as exc:
        sys.exit(f"llm-chat: malformed response: {exc}\n{json.dumps(payload)[:500]}")
    content, reasoning = split_reasoning(message)
    # Don't discard the reasoning: stderr keeps it out of the answer on stdout
    # while leaving it visible to the kb logs for debugging a bad extraction.
    if reasoning and os.environ.get("LLM_EMIT_REASONING") == "1":
        sys.stderr.write(f"llm-chat: reasoning ({len(reasoning)} chars): {reasoning[:2000]}\n")
        sys.stderr.flush()
    sys.stdout.write(content)
    if not content.endswith("\n"):
        sys.stdout.write("\n")


def main() -> None:
    p = argparse.ArgumentParser(
        description="Generic OpenAI-compat chat client.",
        epilog="All flags also readable from env vars (LLM_ENDPOINT, LLM_MODEL, ...).",
    )
    p.add_argument("prompt_pos", nargs="?", metavar="PROMPT",
                   help="Prompt (or use --prompt or stdin)")
    p.add_argument("--prompt", dest="prompt", default=None)
    p.add_argument("--endpoint", default=os.environ.get("LLM_ENDPOINT", "https://api.openai.com/v1"))
    p.add_argument("--model", default=os.environ.get("LLM_MODEL", ""))
    p.add_argument("--api-key", default=os.environ.get("LLM_API_KEY", ""))
    p.add_argument("--system", default=os.environ.get("LLM_SYSTEM", ""))
    p.add_argument("--temperature", type=float,
                   default=float(os.environ.get("LLM_TEMPERATURE", "0.0")))
    p.add_argument("--max-tokens", type=int,
                   default=int(os.environ.get("LLM_MAX_TOKENS", "2048")))
    p.add_argument("--no-thinking", action="store_true",
                   default=os.environ.get("LLM_NO_THINKING", "") == "1")
    p.add_argument("--timeout", type=int, default=int(os.environ.get("LLM_TIMEOUT", "120")))
    p.add_argument("--retries", type=int, default=int(os.environ.get("LLM_RETRIES", "2")))
    p.add_argument("--stream", action="store_true")
    p.add_argument("--json", dest="emit_json", action="store_true",
                   help="Emit the full /v1/chat/completions JSON response")
    args = p.parse_args()

    if not args.model:
        sys.exit("llm-chat: --model or $LLM_MODEL is required")

    prompt = _read_prompt(args.prompt, args.prompt_pos)
    resp = _call_with_retries(args, prompt)

    if args.stream:
        _stream_content(resp)
    else:
        _emit(resp, args.emit_json)


if __name__ == "__main__":
    main()
