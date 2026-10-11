#!/usr/bin/env python3
"""Freeze synthetic conversational evidence through shipping memory APIs.

Run only in an existing disposable memory lab guest. Output contains synthetic
records and source receipts, never credentials or the guest's prior memories.
The companion answer-quality.py evaluates this evidence with real inference.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("native", "cognee", "hillock"), required=True)
    parser.add_argument("--lab-root", type=Path, default=Path("/var/lib/aimee-memory-lab"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    state = json.loads((options.lab_root / "resume-private.json").read_text())
    token = state["kb-supervisor"]["env"]["AIMEE_KB_API_BEARER_TOKEN"]
    project = "conversation-quality-" + str(time.time_ns())
    cases = [
        {"id": "exact_units", "content": "Virant reported: Kibukx is a person whose height is exactly 69cm. Original source: synthetic-message-alpha.", "query": "What height did Virant report for Kibukx?", "required": ["69", "cm", "Virant"], "forbidden": ["69 feet", "69 ft", "mountain"]},
        {"id": "person_mountain", "content": "Samy reported: The Kibukx mountains have an elevation of 69 feet. This place is distinct from the person Kibukx. Original source: synthetic-message-beta.", "query": "What elevation did Samy report for the Kibukx mountains?", "required": ["69", "feet", "Samy"], "forbidden": ["69cm", "69 cm"]},
        {"id": "attribution", "content": "Virant reported: Kibukx's backpack weighs 7kg. Samy later repeated Virant's report; Samy did not make an independent measurement. Original source: synthetic-message-gamma.", "query": "Who originally reported Kibukx's backpack weight? Was Samy independent support?", "required": ["Virant", "Samy"], "forbidden": ["Samy originally", "two independent"]},
        {"id": "conflict", "content": "Virant reported the blue crate weighs 7kg; Samy reported the same blue crate weighs 9kg. These are conflicting unverified speaker claims. Neither has been verified. Sources: synthetic-message-delta and synthetic-message-epsilon.", "query": "What do we know about the blue crate's weight?", "required": ["7", "9", "Virant", "Samy"], "forbidden": ["definitely weighs", "verified weight"]},
        {"id": "negation", "content": "Virant explicitly said: the red box does not contain lithium. Its contents have not been verified. Source: synthetic-message-zeta.", "query": "Did Virant say the red box contains lithium?", "required": ["not", "lithium", "Virant"], "forbidden": ["yes, it contains", "confirmed lithium"]},
        {"id": "fiction", "content": "Samy's claim that the green desk flies was a roleplay joke, not a factual observation. Virant later clarified that the real green desk stays on the floor. Sources: synthetic-message-eta and synthetic-message-theta.", "query": "Does the real green desk fly?", "required": ["floor"], "forbidden": ["can fly", "does fly"]},
        {"id": "topic_return", "content": "Virant and Samy agreed that the observatory review is Tuesday at 14:00 UTC. Virant requested the original time be preserved without a local-time conversion. Source: synthetic-message-iota.", "query": "After our unrelated gardening discussion, what did we agree for the observatory review?", "required": ["Tuesday", "14:00", "UTC"], "forbidden": ["Wednesday", "15:00"]},
        {"id": "paraphrase", "content": "Virant's enduring preference for the laboratory reports is concise answers with metric measurements. Source: synthetic-message-kappa.", "query": "How does Virant want laboratory reports written, and which measurement system?", "required": ["concise", "metric"], "forbidden": ["imperial", "verbose"]},
    ]

    def call(verb, args):
        args = dict(args, project=project, scope_context=True)
        request = subprocess.run(["curl", "-sS", "--max-time", "150", "-H", "Authorization: Bearer " + token,
                                  "-H", "Content-Type: application/json", "--data-binary", json.dumps(args),
                                  "http://127.0.0.1:8741/v1/actions/memory." + verb], capture_output=True, text=True)
        if request.returncode:
            raise RuntimeError("memory transport failed")
        result = json.loads(request.stdout)
        if result.get("status") != "ok":
            raise RuntimeError("memory operation refused: " + str(result.get("error", {}).get("code", "unavailable")))
        return result

    # Native may include shared/global evidence in recall. Select only the
    # fresh fixture's IDs before publishing or feeding the answer model.
    fixture_ids = set()
    for case in cases:
        result = call("store", {"key": project + ":" + case["id"], "content": case["content"], "kind": "fact", "tier": "L2", "confidence": 1})
        fixture_ids.add(result["id"])
    results = []
    for case in cases:
        start = time.monotonic()
        try:
            response = call("recall", {"task_hint": case["query"], "limit_tokens": 1800})
            recall = response["recall"]
            records = []
            for section in ("identity", "preferences", "active_context", "open_commitments"):
                for record in recall.get(section, []):
                    if record.get("id") in fixture_ids:
                        records.append(record)
            found = any(record.get("content") == case["content"] for record in records)
            results.append({"id": case["id"], "query": case["query"], "required": case["required"], "forbidden": case["forbidden"],
                            "records": records, "expected_record_retrieved": found, "recall_ms": round((time.monotonic()-start)*1000, 3),
                            "budget_exceeded": recall.get("budget_exceeded", False), "approx_tokens": recall.get("approx_tokens"),
                            "status": "passed" if found else "failed"})
        except Exception as error:
            results.append({"id": case["id"], "status": "failed", "error": type(error).__name__})
    evidence = {"backend": options.backend, "profile": "shipping-scoped-recall; synthetic multi-speaker claims", "cases": results,
                "passed": all(case["status"] == "passed" for case in results)}
    options.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({"backend": options.backend, "passed": evidence["passed"], "cases": [{"id": c["id"], "status": c["status"]} for c in results]}))
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
