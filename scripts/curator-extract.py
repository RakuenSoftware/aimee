#!/usr/bin/env python3
"""curator-extract.py: deep-curator sidecar for doc/code extraction.

Reads a JSON job description from stdin, calls the local LLM via
llm-chat.py, and writes a JSON artifact list to stdout.

Exit codes: 0 on success, 1 on any error.
"""

import json
import re
import os
import subprocess
import sys
import textwrap


# Backstop cap on extraction input (a symbol body, doc chunk, or scene) sent to
# the LLM. The primary fix is the synth server's context window (see
# aimee-llm-supervisor.sh: AIMEE_LLM_SYNTH_CTX), but a pathological input can still
# exceed any context — and overrunning it makes the server return HTTP 400 and the
# WHOLE extraction fail ("sidecar exited 256"), so a large file would index
# nothing. Cap here so oversized units degrade to a truncated summary instead of
# failing. A summary needs the shape (signature + entry + exit), not every line,
# so keep head+tail and elide the middle. Default ~7K tokens (fits the 32K synth
# default with room for prompt + output); raise via env on big-context deployments.
MAX_INPUT_CHARS = int(os.environ.get("CURATOR_MAX_INPUT_CHARS", "24000"))


def _cap(text: str) -> str:
    if not text or len(text) <= MAX_INPUT_CHARS:
        return text
    dropped = len(text) - MAX_INPUT_CHARS
    head = (MAX_INPUT_CHARS * 3) // 4
    tail = MAX_INPUT_CHARS - head
    # Surface truncation on stderr (stdout carries the JSON artifact) so the kb
    # logs show when a unit was summarized from a truncated body — i.e. when the
    # corpus is being silently under-summarized vs. cleanly indexed.
    sys.stderr.write(
        f"curator-extract: capped input {len(text)}->{MAX_INPUT_CHARS} chars "
        f"({dropped} elided); raise CURATOR_MAX_INPUT_CHARS on a big-context deploy\n"
    )
    sys.stderr.flush()
    return f"{text[:head]}\n\n/* ... {dropped} chars elided to fit the model context ... */\n\n{text[-tail:]}"


def emit_error(msg: str) -> None:
    json.dump({"version": 1, "status": "error", "error": msg}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    sys.exit(1)


def build_doc_prompt(inp: dict) -> str:
    file_path = inp.get("file_path", "")
    heading   = inp.get("heading_path", "") or "(top level)"
    content   = _cap(inp.get("content", ""))
    return textwrap.dedent(f"""
        You are a knowledge extraction assistant. Extract structured knowledge from the
        document chunk below and return ONLY a valid JSON object — no markdown fences.

        Document: {file_path}
        Section:  {heading}

        Content:
        {content}

        Return this exact JSON shape (include only artifacts with confidence >= 0.70;
        always include at least one doc_summary):
        {{
          "artifacts": [
            {{
              "kind": "doc_summary",
              "payload": {{"summary": "one paragraph", "source_file": "{file_path}",
                           "status": "draft|accepted|done|rejected|deferred",
                           "priority": "low|medium|high",
                           "components": ["lowercase-component-tag"]}},
              "confidence": 0.85,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }},
            {{
              "kind": "claim",
              "payload": {{
                "subject": "the thing the claim is about",
                "attribute": "the property/relation being asserted",
                "value": "the asserted value/state",
                "claim_kind": "fact|requirement|constraint|decision|behavior",
                "text": "the full factual claim as one sentence"
              }},
              "confidence": 0.80,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }},
            {{
              "kind": "entity",
              "payload": {{
                "name": "canonical name of a system / component / concept the document references",
                "entity_kind": "component|system|concept|protocol|data_store|tool",
                "context": "short phrase situating how this entity is used here"
              }},
              "confidence": 0.80,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }}
          ]
        }}

        For the doc_summary facets: set "status" from the document's own stated
        status (a literal "Status:" line wins over prose), "priority" from
        explicit priority/severity signals (else "medium"), and "components" to
        1-4 short lowercase tags naming the systems/components the document is
        about. Include one "entity" artifact for each distinct, named system /
        component / concept the document is about (these are mentions to be resolved against the
        canonical entity graph; omit generic words). doc_summary and claim are still
        required.
    """).strip()


def build_code_prompt(inp: dict) -> str:
    file_path = inp.get("file_path", "")
    symbol    = inp.get("symbol", "")
    kind      = inp.get("kind", "function")
    line      = inp.get("line", 0)
    body      = _cap(inp.get("body", ""))
    return textwrap.dedent(f"""
        You are a code analysis assistant. Analyze the {kind} below and return ONLY
        a valid JSON object — no markdown fences.

        File: {file_path}
        Symbol: {symbol} at line {line}

        Body:
        {body}

        Return this exact JSON shape:
        {{
          "artifacts": [
            {{
              "kind": "code_unit",
              "payload": {{
                "summary": "1-2 sentence intent description",
                "invariants": "assumptions and guarantees, or empty string",
                "side_effects": ["filesystem", "allocation"],
                "domain_concepts": ["concept_name"]
              }},
              "confidence": 0.82,
              "citations": []
            }}
          ]
        }}
    """).strip()


def build_story_prompt(inp: dict) -> str:
    """Novel-mode extraction: pull story-world state (entities/edges/facts) out
    of a scene/chapter instead of code symbols or doc claims. Mirrors the
    KB entity/edge/fact graph — no new schema, only a domain-specific prompt."""
    file_path = inp.get("file_path", "")
    heading   = inp.get("heading_path", "") or "(top level)"
    content   = _cap(inp.get("content", ""))
    return textwrap.dedent(f"""
        You are a story-continuity analyst for a work of fiction. Extract the
        story-world state established by the scene below and return ONLY a valid
        JSON object — no markdown fences.

        Manuscript: {file_path}
        Scene:      {heading}

        Content:
        {content}

        Extract three artifact kinds (include only artifacts with confidence >= 0.70):
          - "entity": a character, location, faction, item, or event. payload has
            "entity_type" (one of character|location|faction|item|event), "name",
            and a one-line "summary".
          - "edge": a relationship between two named entities. payload has "src",
            "dst", and "relation" (e.g. knows, located_in, member_of, parent_of,
            enemy_of, present_at).
          - "fact": a canon statement the scene establishes ("Mara's eyes are
            grey"). payload has "text" and "provenance" (the chapter/scene that
            established it; use "{heading}" if unsure).

        Always include at least one entity. Return this exact JSON shape:
        {{
          "artifacts": [
            {{
              "kind": "entity",
              "payload": {{"entity_type": "character", "name": "Mara",
                          "summary": "one-line description"}},
              "confidence": 0.90,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }},
            {{
              "kind": "edge",
              "payload": {{"src": "Mara", "dst": "Riverford", "relation": "located_in"}},
              "confidence": 0.80,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }},
            {{
              "kind": "fact",
              "payload": {{"text": "Mara's eyes are grey", "provenance": "{heading}"}},
              "confidence": 0.85,
              "citations": [{{"span_start": 0, "span_end": 0}}]
            }}
          ]
        }}
    """).strip()


def call_llm(prompt: str, max_tokens: int) -> tuple[str | None, str | None]:
    # Offline test seam: when CURATOR_LLM_STUB_FILE points at a readable file,
    # return its contents verbatim instead of calling the live LLM. Lets the
    # extraction contract be exercised deterministically in CI (see
    # scripts/check-curator-story.py) with no model or network dependency.
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
    env["LLM_JSON_MODE"]   = "1"
    env["LLM_MAX_TOKENS"]  = str(max_tokens)
    env.setdefault("LLM_ENDPOINT", "http://192.168.1.122:8080")
    env.setdefault("LLM_MODEL",    "qwen3")

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


def strip_fences(text: str) -> str:
    text = text.strip()
    # Reasoning models (e.g. MiniMax-M3, Qwen3) prepend a <think>...</think>
    # block; keep only what follows the final close tag.
    if "</think>" in text:
        text = text.rsplit("</think>", 1)[1].strip()
    if text.startswith("```"):
        lines = text.splitlines()
        end = len(lines) - 1 if lines[-1].strip() == "```" else len(lines)
        text = "\n".join(lines[1:end]).strip()
    # Extract the first balanced top-level {...} object (string-aware), dropping
    # any prose before or after it.
    text = _first_json_object(text)
    return text.strip()


def _first_json_object(text: str) -> str:
    start = text.find("{")
    if start < 0:
        return text
    depth = 0
    in_str = False
    esc = False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return text[start:]


def main() -> None:
    try:
        req = json.load(sys.stdin)
    except Exception as exc:
        emit_error(f"invalid JSON input: {exc}")

    role       = req.get("role", "")
    inp        = req.get("input", {})
    max_tokens = req.get("config", {}).get("max_tokens", 2048)

    if role == "extract_doc":
        prompt = build_doc_prompt(inp)
    elif role == "extract_code_unit":
        prompt = build_code_prompt(inp)
    elif role == "extract_story":
        prompt = build_story_prompt(inp)
    else:
        emit_error(f"unknown role: {role!r}")

    raw, llm_err = call_llm(prompt, max_tokens)
    if llm_err:
        emit_error(llm_err)

    text = strip_fences(raw or "")

    try:
        parsed = json.loads(text)
    except Exception:
        # Tolerate trailing commas before } or ] (a common LLM JSON error).
        repaired = re.sub(r",(\s*[}\]])", r"\1", text)
        try:
            parsed = json.loads(repaired)
        except Exception as exc:
            emit_error(f"LLM returned non-JSON: {exc}: {text[:300]}")

    artifacts = parsed.get("artifacts", [])
    if not isinstance(artifacts, list):
        emit_error("artifacts must be a list")

    json.dump({"version": 1, "status": "ok", "artifacts": artifacts}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
