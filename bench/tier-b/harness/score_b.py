"""Score Tier-B syntheses: format, faithfulness, coverage.

No reference-answer matching. There is no single correct paragraph, and the
Tier-A experience — nine defects, most of them the grader punishing a correct
answer phrased differently — is a strong argument against building another
matcher for free text.

Instead, three properties taken from the prompt's own instructions:

  format        production's parse succeeds and yields a non-empty synthesis
  faithfulness  "grounded only in those sources": every capitalised entity,
                number and identifier in the output appears in the sources
  coverage      the required facts are present, in any of their accepted forms

Faithfulness is the one that matters. A synthesis is written to the artifact
store and cited, so an invented fact is durable and arrives wearing citations
that appear to support it.
"""

import argparse
import json
import re

# Words that start sentences or are otherwise capitalised without being entities.
STOPCAPS = {
    "the", "a", "an", "it", "its", "this", "that", "these", "those", "there",
    "they", "their", "he", "she", "his", "her", "we", "our", "you", "your",
    "and", "but", "or", "if", "when", "while", "however", "although", "based",
    "according", "as", "at", "by", "for", "from", "in", "into", "of", "on", "to",
    "with", "no", "not", "both", "each", "all", "some", "one", "two", "three",
    "tier", "source", "sources", "note", "notes", "additionally", "further",
    "overall", "finally", "currently", "also", "is", "was", "are", "were",
}


def norm(s):
    return re.sub(r"\s+", " ", (s or "").casefold()).strip()


def parse_synthesis(raw):
    """Mirror the production parse in kb_curator_synthesize.c.

    First '{' to last '}', then the "synthesis" key, falling back to "text" —
    production accepts both. Returns (text, format_ok).
    """
    if not raw:
        return "", False
    start, end = raw.find("{"), raw.rfind("}")
    if start == -1 or end < start:
        return "", False
    try:
        obj = json.loads(raw[start:end + 1])
    except json.JSONDecodeError:
        return "", False
    if not isinstance(obj, dict):
        return "", False
    text = obj.get("synthesis")
    if not isinstance(text, str) or not text:
        text = obj.get("text")
    if not isinstance(text, str) or not text.strip():
        return "", False
    return text, True


def source_text(row):
    """Everything the model was shown, flattened for tracing."""
    parts = [row["topic"]["name"], row["topic"]["id"]]
    for s in row["sources"]:
        parts += [s["id"], s["kind"]]
        pl = s["payload"]
        parts += [str(v) for v in (pl.values() if isinstance(pl, dict) else [pl])]
    return norm(" ".join(parts))


def claims_in(text):
    """Entities, numbers and identifiers a synthesis asserts.

    Capitalised words (not sentence-initial stopwords), digit runs, dotted
    identifiers and version-like strings. Deliberately narrow: it catches
    invented names and numbers, which is the failure that matters for a cited
    artifact, and does not attempt to catch an invented *relationship* between
    two entities that both genuinely appear.
    """
    out = set()
    for m in re.finditer(r"\b[A-Z][A-Za-z0-9._-]{2,}\b", text):
        w = m.group(0)
        if w.casefold() in STOPCAPS:
            continue
        out.add(w.casefold())
    for m in re.finditer(r"\b\d[\d.,]*\b", text):
        out.add(m.group(0).rstrip(".,").casefold())
    return out


def traceable(claim, src):
    """Is this token present in the sources, allowing for punctuation drift?"""
    if claim in src:
        return True
    bare = claim.replace(",", "")
    if bare and bare in src.replace(",", ""):
        return True
    # A hyphenated or dotted compound counts if its parts are all present.
    parts = [p for p in re.split(r"[._-]+", claim) if len(p) > 2]
    return bool(parts) and all(p in src for p in parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--topics", required=True)
    ap.add_argument("--pred", required=True)
    ap.add_argument("--json-out")
    args = ap.parse_args()

    topics = {json.loads(l)["id"]: json.loads(l)
              for l in open(args.topics) if l.strip()}
    preds = [json.loads(l) for l in open(args.pred) if l.strip()]
    if len(preds) != len(topics):
        raise SystemExit(
            f"incomplete predictions: {args.pred} has {len(preds)} rows, "
            f"topics has {len(topics)}. Re-run or delete the partial file.")
    # A row that hit the harness's own --max-tokens cap is a harness artifact, not
    # a model result: production allows CURATOR_SYNTH_OUTBUF (16384), so a response
    # cut off below that would not have been cut off in the deployment being
    # measured. Scoring it as a format failure charges the model for my bound.
    # gemma-4-12B lost format 1.0 -> 0.833 and coverage 1.0 -> 0.75 to exactly one
    # such row before this check existed.
    cut = [p["id"] for p in preds if p.get("truncated")]
    if cut:
        raise SystemExit(
            f"truncated predictions: {args.pred} rows {cut} hit --max-tokens. "
            f"Re-run those with a higher cap; scoring them would charge the model "
            f"for the harness bound.")

    per, fmt_ok = [], 0
    cov_hit = cov_tot = 0
    inv_tot = claim_tot = 0
    for p in preds:
        row = topics[p["id"]]
        text, ok = parse_synthesis(p.get("raw", ""))
        fmt_ok += ok
        src = source_text(row)
        low = norm(text)

        covered = [any(f in low for f in c["forms"]) for c in row["must_cover"]]
        cov_hit += sum(covered)
        cov_tot += len(covered)

        claims = claims_in(text)
        invented = sorted(c for c in claims if not traceable(c, src))
        inv_tot += len(invented)
        claim_tot += len(claims)

        per.append({
            "id": p["id"], "category": row["category"], "format_ok": ok,
            "words": len(text.split()),
            "coverage": round(sum(covered) / len(covered), 3) if covered else None,
            "missed": [c["fact"] for c, hit in zip(row["must_cover"], covered) if not hit],
            "checked_claims": len(claims),
            "invented": invented,
            "latency_ms": p.get("latency_ms"),
        })

    by_cat = {}
    for r in per:
        b = by_cat.setdefault(r["category"], {"n": 0, "cov": 0.0, "inv": 0})
        b["n"] += 1
        b["cov"] += r["coverage"] or 0.0
        b["inv"] += len(r["invented"])

    report = {
        "model": preds[0].get("model"),
        "topics": len(preds),
        "format_rate": round(fmt_ok / len(preds), 4),
        "coverage": round(cov_hit / cov_tot, 4) if cov_tot else None,
        "faithfulness": round(1 - inv_tot / claim_tot, 4) if claim_tot else None,
        "invented_claims": inv_tot,
        "checked_claims": claim_tot,
        "median_words": sorted(r["words"] for r in per)[len(per) // 2],
        "median_latency_ms": sorted(
            (r["latency_ms"] or 0) for r in per)[len(per) // 2],
        "by_category": {k: {"n": v["n"], "coverage": round(v["cov"] / v["n"], 3),
                            "invented": v["inv"]} for k, v in sorted(by_cat.items())},
        "per_topic": per,
    }
    out = json.dumps(report, indent=2)
    print(out)
    if args.json_out:
        open(args.json_out, "w").write(out + "\n")


if __name__ == "__main__":
    main()
