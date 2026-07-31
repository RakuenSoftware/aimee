"""Unit tests for the scorer, with known inputs and known answers.

Every check on this scorer so far has been an audit of its output on real model
predictions — which finds bugs only where a model happened to trip one. This
feeds it constructed cases where the correct P/R/F1 is arithmetic, so a defect
shows up whether or not a model would have exercised it.

Run: python3 harness/test_score.py
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
GOLD = HERE.parent / "data" / "gold.jsonl"


def score(preds, gold_rows=None, extra=()):
    """Score a synthetic prediction set against a synthetic gold set."""
    with tempfile.TemporaryDirectory() as d:
        g = Path(d) / "g.jsonl"
        p = Path(d) / "p.jsonl"
        g.write_text("\n".join(json.dumps(r) for r in gold_rows) + "\n")
        p.write_text("\n".join(json.dumps(r) for r in preds) + "\n")
        out = subprocess.run(
            [sys.executable, str(HERE / "score.py"), "--gold", str(g), "--pred", str(p), *extra],
            capture_output=True, text=True, cwd=HERE)
        if out.returncode != 0:
            raise AssertionError(out.stderr.strip() or "scorer failed")
        return json.loads(out.stdout)


def T(s, r, o, **kw):
    return dict(subject=s, relation=r, object=o, **kw)


def G(nid, note, triples, cat="third_person"):
    return {"id": nid, "category": cat, "note": note, "gold": triples}


def P(nid, triples):
    return {"id": nid, "model": "test", "pred": triples, "pred_nofloor": triples,
            "raw": json.dumps({"facts": triples}), "parse_ok": True, "schema_ok": True}


def near(a, b, what):
    # The scorer rounds reported figures to 4dp, so compare at that precision.
    assert abs(a - b) < 5e-5, f"{what}: expected {b}, got {a}"


def test_perfect():
    g = [G("a", "Marta lives in Lisbon.", [T("Marta", "lives_in", "Lisbon")])]
    r = score([P("a", [T("Marta", "lives_in", "Lisbon", confidence=0.9)])], g)
    near(r["strict"]["f1"], 1.0, "exact match F1")
    near(r["strict"]["precision"], 1.0, "precision")
    near(r["strict"]["recall"], 1.0, "recall")
    print("  PASS: exact match scores 1.0")


def test_empty_prediction():
    g = [G("a", "Marta lives in Lisbon.", [T("Marta", "lives_in", "Lisbon")])]
    r = score([P("a", [])], g)
    near(r["strict"]["f1"], 0.0, "empty prediction F1")
    assert r["strict"]["fn"] == 1, "the missed triple must count as a false negative"
    print("  PASS: empty prediction scores 0 with one FN")


def test_one_spurious():
    """n correct + 1 invented: precision n/(n+1), recall 1."""
    g = [G("a", "Marta lives in Lisbon and works for Corvo.",
           [T("Marta", "lives_in", "Lisbon"), T("Marta", "works_for", "Corvo")])]
    r = score([P("a", [T("Marta", "lives_in", "Lisbon"), T("Marta", "works_for", "Corvo"),
                       T("Marta", "spouse", "Lisbon")])], g)
    near(r["strict"]["precision"], 2 / 3, "precision with one spurious")
    near(r["strict"]["recall"], 1.0, "recall unaffected by a spurious triple")
    print("  PASS: one spurious triple costs precision only")


def test_one_missed():
    g = [G("a", "Marta lives in Lisbon and works for Corvo.",
           [T("Marta", "lives_in", "Lisbon"), T("Marta", "works_for", "Corvo")])]
    r = score([P("a", [T("Marta", "lives_in", "Lisbon")])], g)
    near(r["strict"]["precision"], 1.0, "precision unaffected by a miss")
    near(r["strict"]["recall"], 0.5, "recall with one missed")
    print("  PASS: one missed triple costs recall only")


def test_symmetric_swap():
    """The ontology declares spouse symmetric, so argument order is free."""
    g = [G("a", "Sarah is my wife.", [T("user", "spouse", "Sarah")], cat="first_person")]
    r = score([P("a", [T("Sarah", "spouse", "user")])], g)
    near(r["strict"]["f1"], 1.0, "symmetric swap must score as correct")
    print("  PASS: symmetric relation accepts either argument order")


def test_inverse():
    """parent_of/child_of is an auto-enforced inverse pair."""
    g = [G("a", "Priya is Anand's daughter.", [T("Anand", "parent_of", "Priya")])]
    r = score([P("a", [T("Priya", "child_of", "Anand")])], g)
    near(r["strict"]["f1"], 1.0, "inverse direction must score as correct")
    print("  PASS: inverse relation accepts the converse direction")


def test_asymmetric_swap_is_wrong():
    """works_for is NOT symmetric: swapping it asserts something false."""
    g = [G("a", "Marta works for Corvo.", [T("Marta", "works_for", "Corvo")])]
    r = score([P("a", [T("Corvo", "works_for", "Marta")])], g)
    near(r["strict"]["f1"], 0.0, "swapping an asymmetric relation must NOT score")
    print("  PASS: swapping an asymmetric relation is not credited")


def test_endpoint_change_is_wrong():
    """The rule that took three passes to get right: endpoints must match."""
    g = [G("a", "The build host is called forge.", [T("forge", "device_has_ip", "10.0.0.1")],
           cat="infra")]
    r = score([P("a", [T("build host", "device_has_ip", "10.0.0.1")])], g)
    near(r["strict"]["f1"], 0.0, "a renamed endpoint is a different fact")
    print("  PASS: renaming an endpoint is not credited")


def test_surface_variation_is_free():
    g = [G("a", "Dr. Okafor works for St Vincent's.",
           [T("Dr. Okafor", "works_for", "St Vincent's")])]
    for variant in ("Okafor", "dr okafor", "DR. OKAFOR"):
        r = score([P("a", [T(variant, "works_for", "St Vincent's")])], g)
        near(r["strict"]["f1"], 1.0, f"honorific/case variant {variant!r}")
    print("  PASS: honorific and case variation is not a difference")


def test_predicate_equivalence():
    g = [G("a", "I speak fluent Japanese.", [T("user", "speaks", "Japanese")],
           cat="novel_pred")]
    r = score([P("a", [T("user", "speaks_language", "Japanese")])], g)
    near(r["strict"]["f1"], 1.0, "equivalent predicate")
    print("  PASS: equivalent predicates are accepted")


def test_unrelated_predicate_is_wrong():
    g = [G("a", "Marta works for Corvo.", [T("Marta", "works_for", "Corvo")])]
    r = score([P("a", [T("Marta", "spouse", "Corvo")])], g)
    near(r["strict"]["f1"], 0.0, "unrelated predicate must not match")
    print("  PASS: an unrelated predicate is not credited")


def test_duplicate_prediction():
    """A repeated triple can be credited once; the copy is a false positive."""
    g = [G("a", "Marta lives in Lisbon.", [T("Marta", "lives_in", "Lisbon")])]
    r = score([P("a", [T("Marta", "lives_in", "Lisbon"), T("Marta", "lives_in", "Lisbon")])], g)
    assert r["strict"]["tp"] == 1, "a duplicate must not be counted twice"
    assert r["strict"]["fp"] == 1, "the duplicate copy is a false positive"
    print("  PASS: duplicates credited once")


def test_empty_gold_note():
    """A factless note: any triple is a false positive, none are false negatives."""
    g = [G("a", "Feeling burnt out this week.", [], cat="transient")]
    r = score([P("a", [T("user", "has_role", "burnt out")])], g)
    assert r["strict"]["fp"] == 1 and r["strict"]["fn"] == 0, "factless note accounting"
    near(r["over_extraction"]["abstention_rate_on_schema"], 0.0, "abstention")
    print("  PASS: factless notes charge precision only")


def test_abstention_counts_terse_empty():
    """{} on a factless note is a correct abstention, not a schema failure."""
    g = [G("a", "Feeling burnt out this week.", [], cat="transient")]
    p = P("a", [])
    p["raw"] = "{}"
    r = score([p], g)
    near(r["over_extraction"]["abstention_rate_on_schema"], 1.0, "terse empty abstention")
    near(r["output_health"]["schema_rate"], 1.0, "{} is schema-valid")
    print("  PASS: a terse {} counts as abstention, not malformed")


def test_conf_floor_is_no_longer_the_default_gate():
    """The default view must be the gate src/kb/kb_memory_facts.c applies today.

    MF_CONF_FLOOR was removed and replaced by fact_grounded(); the scorer kept
    defaulting to the floored view for a while afterwards, which reported
    Qwen3-0.6B as F1 0.0000 when it had extracted 72 triples the shipping code
    would have committed. The floored view still exists, behind an explicit flag,
    because the historical sweeps were scored with it.
    """
    g = [G("a", "Marta lives in Lisbon.", [T("Marta", "lives_in", "Lisbon")])]
    p = P("a", [T("Marta", "lives_in", "Lisbon", confidence=0.1)])
    p["pred"] = []                      # what the retired floor would commit
    r_default = score([p], g)
    r_floor = score([p], g, extra=("--pred-key", "pred"))
    r_cap = score([p], g, extra=("--pred-key", "pred_nofloor"))
    near(r_default["strict"]["f1"], 1.0,
         "default view keeps a grounded fact regardless of self-reported confidence")
    near(r_floor["strict"]["f1"], 0.0, "the retired floored view still drops it")
    near(r_cap["strict"]["f1"], 1.0, "ungated view keeps it")
    print("  PASS: default is the grounding gate, floor available explicitly")


def test_grounding_gate_drops_an_invented_endpoint():
    """The default gate must drop what production drops: an endpoint that cannot
    be traced to the note. A well-formed triple about someone never mentioned is
    the failure the write gate cannot catch."""
    g = [G("a", "Marta lives in Lisbon.", [T("Marta", "lives_in", "Lisbon")])]
    p = P("a", [T("Marta", "lives_in", "Lisbon", confidence=0.9),
                T("Bartholomew", "lives_in", "Reykjavik", confidence=0.9)])
    r = score([p], g)
    near(r["strict"]["precision"], 1.0, "the invented triple is gated out, not scored")
    print("  PASS: grounding gate drops an ungrounded endpoint")


def test_degenerate_entities():
    """Normalisation must never destroy a value or invent one.

    Article stripping used to empty a subject legitimately named "a", and
    ground_text(None) produced the literal string "none" — which can match a note
    containing that word.
    """
    import sys as _sys
    _sys.path.insert(0, str(HERE))
    import score as S

    assert S.norm_entity("a") == "a", "a single-article entity must survive"
    assert S.norm_entity("the") == "the"
    assert S.norm_entity("the KB") == "kb", "a leading article is still stripped"
    assert S.norm_entity("Dr. Okafor") == "okafor"
    assert S.norm("a") == "a"
    assert S.ground_text(None) == "", "None must not become the word 'none'"
    print("  PASS: degenerate entities are not destroyed or invented")


def test_incomplete_file_refused():
    g = [G("a", "x", [T("A", "knows", "B")]), G("b", "y", [T("C", "knows", "D")])]
    try:
        score([P("a", [])], g)
    except AssertionError as e:
        assert "incomplete" in str(e).lower(), f"unexpected error: {e}"
        print("  PASS: an incomplete prediction file is refused")
        return
    raise AssertionError("scorer accepted an incomplete prediction file")


if __name__ == "__main__":
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    print("scorer: all tests passed")
