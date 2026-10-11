"""Rebuild useful conversation state from eligible, attributed source episodes.

Labels describe explicit speaker language; they do not verify claims or infer
agreement from model responses. Durable projection rows contain source pointers,
so deletion/private-scope changes cannot leave copied claims in the projection.
"""
import json
import re
import time

PROJECTION_KINDS = {"recent_human_context": "scratch", "purpose": "task", "decisions": "decision",
    "open_questions": "hypothesis", "constraints": "policy", "feedback": "opinion", "preferences": "preference"}

CATEGORY_PATTERNS = {
    "recent_human_context": r"(?s).",
    "purpose": r"\b(?:our goal|the goal|our purpose|we want to|we need to|let's work on)\b",
    "decisions": r"\b(?:we agreed|we decided|our decision|let's use|the decision is)\b",
    "open_questions": r"\?$|\b(?:open question|still need to decide|unresolved)\b|\b(?:resolved|answer):",
    "constraints": r"\b(?:must|do not|don't|keep|only|correction|actually|not .*but)\b",
    "feedback": r"\b(?:your (?:answer|reply)|that (?:answer|reply|was) (?:is )?(?:wrong|incorrect)|you got|correction:)\b",
    "preferences": r"\b(?:i prefer|please (?:respond|reply|use|keep)|my preference|forget (?:my |the )?preference)\b",
}


def annotations(episode):
    if episode.get("speaker_kind") != "human":
        return []
    text = episode["text"].strip()
    # Quoted, fictional and joking utterances remain raw episodes, never controls.
    if re.search(r"\b(?:joking|just kidding|in (?:the |our )?(?:story|roleplay|fiction)|quote:)\b", text, re.I):
        return []
    now = episode["observed_unix_ms"] / 1000
    values = []
    for category, pattern in CATEGORY_PATTERNS.items():
        if re.search(pattern, text, re.I):
            item = {"category": category, "observed_at": now, "speaker_id": episode["speaker_id"]}
            if category == "open_questions":
                item["resolved"] = bool(re.search(r"\b(?:resolved|answer):", text, re.I))
                item["reply_to_message_id"] = episode.get("reply_to_message_id", "")
            if category == "preferences":
                duration = re.search(r"(?:for (?:the )?next|for) (\d+) (seconds?|minutes?|hours?|days?)", text, re.I)
                seconds = 86400
                if duration:
                    seconds = min(int(duration[1]) * {"second": 1, "minute": 60, "hour": 3600, "day": 86400}[duration[2].lower().rstrip("s")], 30*86400)
                item["expires_at"] = now + seconds
                item["lifetime"] = "explicit duration" if duration else "channel and speaker scoped; default 24 hours"
                item["revoked"] = bool(re.search(r"forget (?:my |the )?preference", text, re.I))
            values.append(item)
    return values


async def rebuild(action, project, pointers, now=None):
    now = time.time() if now is None else now
    projection = {name: [] for name in CATEGORY_PATTERNS}
    projection["partners"] = []
    sources, revoked, seen = [], {}, set()
    decoded = []
    source_cache = {}
    for row in pointers:
        if not str(row.get("key", "")).startswith("discord-projection:"):
            continue
        try:
            pointer = json.loads(row["content"])
        except (ValueError, KeyError, TypeError):
            raise RuntimeError("invalid conversation projection pointer") from None
        if pointer.get("schema_version") != 1 or pointer.get("project") != project or type(pointer.get("source_id")) is not int:
            raise RuntimeError("conversation projection scope mismatch")
        decoded.append(pointer)
    async def eligible(pointer):
        if pointer["source_id"] not in source_cache:
            source_cache[pointer["source_id"]] = await action("memory.get", {"project": project, "scope_context": True,
                "id": pointer["source_id"], "include_version": True})
        current = source_cache[pointer["source_id"]]
        if current.get("kind") == "not_found" or current.get("status") == "not_found":
            return None
        record = current.get("memory")
        if not isinstance(record, dict):
            raise RuntimeError("conversation projection source unavailable")
        try:
            episode = json.loads(record["content"])
        except (ValueError, KeyError, TypeError):
            raise RuntimeError("conversation projection source invalid") from None
        if (record["id"] != pointer["source_id"] or episode.get("source") != pointer["source"]
                or episode.get("speaker_kind") != "human" or episode.get("speaker_id") != pointer["speaker_id"]
                or record.get("provenance_category") != "user_stated"
                or not record.get("key", "").startswith(project + ":")):
            raise RuntimeError("conversation projection source identity changed")
        # Pointer content is model-authored derivation. Recompute every control,
        # lifetime and category from the eligible human source before using it.
        expected = [{**item, "schema_version": 1, "project": project,
                     "source_id": record["id"], "source": episode["source"]} for item in annotations(episode)]
        if pointer not in expected:
            raise RuntimeError("conversation projection differs from its source")
        return record, episode

    # Revocation and resolution need the same source checks as displayed claims.
    resolved = set()
    for pointer in decoded:
        if pointer.get("category") == "preferences" and pointer.get("revoked"):
            if await eligible(pointer):
                revoked[pointer["speaker_id"]] = max(revoked.get(pointer["speaker_id"], 0), pointer["observed_at"])
        if pointer.get("category") == "open_questions" and pointer.get("resolved") and pointer.get("reply_to_message_id"):
            if await eligible(pointer):
                resolved.add(pointer["reply_to_message_id"])
    for pointer in sorted(decoded, key=lambda p: p["observed_at"], reverse=True):
        category = pointer.get("category")
        if category not in CATEGORY_PATTERNS or pointer.get("revoked") or pointer.get("resolved") or (category == "open_questions" and pointer["source"].rsplit(":", 1)[-1] in resolved):
            continue
        if category == "preferences" and (pointer["expires_at"] <= now or pointer["observed_at"] <= revoked.get(pointer["speaker_id"], 0)):
            continue
        if category == "purpose" and projection[category]:
            continue
        if len(projection[category]) >= 4:
            continue
        result = await eligible(pointer)
        if result is None:
            continue
        record, episode = result
        item = {"source_id": record["id"], "speaker_id": episode["speaker_id"],
            "speaker_name": episode["speaker_name"], "text": episode["text"], "status": "unverified speaker statement"}
        if category == "preferences":
            item.update(expires_at=pointer["expires_at"], lifetime=pointer["lifetime"])
        projection[category].append(item)
        if record["id"] not in seen:
            sources.append(record);seen.add(record["id"])
    return projection, sources
