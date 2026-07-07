#!/usr/bin/env python3
"""Probe candidate queries across fusion modes; flag where the top result DIFFERS.
Those are the adversarial cases where the lexical vs dense legs disagree and the
alpha choice actually re-ranks. Purely diagnostic (no scoring/labels)."""
import json, sys, urllib.request

EP = "http://192.168.1.254:8741/v1/search"
MODES = ["rrf", "static_alpha", "dynamic_alpha"]
CANDIDATES = [
    "economizer", "vulkan", "context fold", "rolling-fold fold-free",
    "call graph symbol index", "trust on first use bearer enrollment",
    "pgvector postgres collection", "delegate roster local codex claude",
    "reciprocal rank fusion trust weighting", "curator drain lease wedge",
    "AIMEE_WEBCHAT_USERS", "kb_fusion_static_alpha", "locomo bm25 parity",
    "browser dashboard logs tab", "embedder model selection sweep",
    "how to keep sessions from exceeding the context window",
    "stop confirming every action", "which GPUs and operating systems work",
    "hand it a proposal and it builds the change", "quantization gguf model tier",
    "how are secrets and tokens kept out of logs", "PAM login user provisioning",
]

def top(query, mode, k=5):
    body = json.dumps({"query": query, "project": "aimee", "max_results": k,
                       "fusion_mode": mode}).encode()
    req = urllib.request.Request(EP, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            hits = json.loads(r.read().decode()).get("hits", [])
    except Exception as e:
        return "ERR:%s" % e
    return hits[0]["artifact_id"] if hits else "(none)"

diffs = 0
for q in CANDIDATES:
    tops = {m: top(q, m) for m in MODES}
    uniq = set(tops.values())
    flag = "  <<< DIFFERS" if len(uniq) > 1 else ""
    if flag:
        diffs += 1
    print("q=%-52s rrf=%-34s dyn=%-34s%s"
          % (q[:52], tops["rrf"].split("/")[-1][:34], tops["dynamic_alpha"].split("/")[-1][:34], flag))
    if flag:
        print("      static_alpha=%s" % tops["static_alpha"])
print("\n%d/%d candidates differentiate the modes" % (diffs, len(CANDIDATES)))
