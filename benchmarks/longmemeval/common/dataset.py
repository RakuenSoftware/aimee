#!/usr/bin/env python3
"""LongMemEval dataset helpers."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from benchmarks.common.dataset_inventory import DatasetCases


def _infer_subset(item: dict[str, Any], question: str) -> str:
    for key in ("subset", "question_type", "category", "type"):
        value = item.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip().lower().replace(" ", "-")
    lowered = question.lower()
    if any(word in lowered for word in ("when", "before", "after")):
        return "temporal-reasoning"
    if "prefer" in lowered or "favorite" in lowered:
        return "single-session-preference"
    if "update" in lowered or "now" in lowered:
        return "knowledge-update"
    return "default"


def load_cases(dataset_path: str, max_cases: int = 0) -> list[dict[str, Any]]:
    root = json.loads(Path(dataset_path).read_text())
    if max_cases < 0:
        raise ValueError("negative case cap")
    cases = DatasetCases(dataset_path)
    for index, item in enumerate(root):
        case_id = str(item.get("question_id") or f"longmemeval-{index + 1}")
        label = item.get("answerable", "_abs" not in case_id)
        if type(label) is not bool:
            raise ValueError("invalid answerability label")
        question = str(item.get("question", "")).strip()

        answer = item.get("answer", "")
        sessions = item.get("haystack_sessions", [])
        session_ids = item.get("haystack_session_ids", [])
        dates = item.get("haystack_dates", [])
        answer_ids = [str(entry) for entry in item.get("answer_session_ids", []) if isinstance(entry, str)]
        reason = ("max_cases" if max_cases and len(cases) >= max_cases else
                  "empty_question" if not question else "empty_history" if not sessions else "")
        cases.record(case_id, label, reason)
        if reason:
            continue
        normalized_sessions = []
        for idx, session in enumerate(sessions):
            normalized_sessions.append(
                {
                    "session_id": str(session_ids[idx] if idx < len(session_ids) else f"session-{idx + 1}"),
                    "date_time": str(dates[idx] if idx < len(dates) else ""),
                    "turns": session,
                }
            )
        cases.append(
            {
                "question_id": case_id,
                "answerable": label,
                "question": question,
                "gold_answer": str(answer),
                "subset": _infer_subset(item, question),
                "sessions": normalized_sessions,
                "answer_session_ids": answer_ids,
            }
        )
    return cases
