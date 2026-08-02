"""Score Tier-A triple extraction against the gold set.

Reads a predictions JSONL (one row per note, produced by a runner) and the gold
set, and emits the metrics the proposal's §4.2 asks for: triple
precision/recall/F1, plus the over-extraction rate that matters most for a drain
that commits into memory_facts.

Matching is greedy 1-1 within a note. Subject and relation must match exactly
after normalization under both modes; only object comparison loosens in lenient
mode. See data/LABELING.md.
"""

import argparse
import json
import re
from collections import defaultdict

import prompt

ARTICLES = {"the", "a", "an"}

# Honorifics are surface, not identity: "Dr. Okafor" and "Okafor" are one person.
# Models drop them routinely and were charged a false positive and a false
# negative each time.
HONORIFICS = {"dr", "mr", "mrs", "ms", "miss", "prof", "professor", "sir", "rev"}

# Models emit ages and counts as words as readily as digits ("Nina is seven").
# The ontology stores a scalar either way, so treating them as different answers
# measures spelling, not extraction.
NUMBER_WORDS = {
    "zero": "0", "one": "1", "two": "2", "three": "3", "four": "4", "five": "5",
    "six": "6", "seven": "7", "eight": "8", "nine": "9", "ten": "10",
    "eleven": "11", "twelve": "12",
}


def norm(s):
    if s is None:
        return ""
    s = str(s).casefold().strip()
    s = re.sub(r"\s+", " ", s)
    if s in NUMBER_WORDS:
        return NUMBER_WORDS[s]
    # Strip edge punctuation but preserve internal dots/colons so IPs and
    # hostnames survive intact.
    s = re.sub(r"^[^\w]+|[^\w]+$", "", s)
    toks = s.split()
    # Never strip the value away entirely: an entity legitimately named "A"
    # would otherwise normalise to the empty string and match nothing.
    while len(toks) > 1 and toks[0] in ARTICLES:
        toks.pop(0)
    return " ".join(toks)


def norm_entity(s):
    """Normalise a subject or object for comparison.

    norm() preserves underscores, hyphens and internal punctuation, so kb_server
    did not match "KB server", aimee-kb did not match "aimee kb", and
    "Dr. Okafor" did not match "Dr Okafor". Models write snake_case endpoints
    constantly, and each of those counted as both a false positive and a false
    negative — the same failure mode as the symmetry and inverse bugs.

    Internal dots survive so 192.168.1.253 stays intact; trailing punctuation is
    stripped per token so a sentence-final "Wellington." meets "Wellington".

    Applied to endpoints only. Relations keep norm() plus rel_type_canonicalize,
    because snake_case IS the canonical form for a predicate.
    """
    s = re.sub(r"[_\-/]+", " ", str(s or "").casefold())
    s = re.sub(r"\s+", " ", s).strip()
    toks = []
    for t in s.split():
        t = t.strip(",;:!?()[]{}\"'")
        t = re.sub(r"\.+$", "", t)  # trailing dots only; keep 192.168.1.1
        t = NUMBER_WORDS.get(t, t)
        if t:
            toks.append(t)
    while len(toks) > 1 and (toks[0] in ARTICLES or toks[0] in HONORIFICS):
        toks.pop(0)
    return " ".join(toks)


def tok_f1(a, b):
    ta, tb = set(a.split()), set(b.split())
    if not ta or not tb:
        return 1.0 if ta == tb else 0.0
    inter = len(ta & tb)
    if not inter:
        return 0.0
    p, r = inter / len(ta), inter / len(tb)
    return 2 * p * r / (p + r)


def entity_match(pred, gold, lenient):
    """Endpoint comparison, applied identically to subjects and objects.

    Containment was originally on objects only, which was an accident of where
    the failing cases happened to appear rather than a principle. It currently
    changes nothing on either side, but treating the two endpoints differently
    would produce a surprising result the first time a model elaborated a subject
    the way they routinely elaborate objects.
    """
    return obj_match(pred, gold, lenient)


def obj_match(pred, gold, lenient):
    if pred == gold:
        return True
    # Containment applies in BOTH modes. A prediction that fully contains the gold
    # object names the same thing with more words, and in the cases that surfaced
    # it was more faithful to the note than the label: "2 of the junior engineers"
    # where the note says "mentors two of the junior engineers", "nordkraft board"
    # where the note says "on the board at Nordkraft". Penalising the model for
    # being more specific than my under-specified gold measures the labeller.
    # Requires the gold side covered and non-trivial, so a single shared word
    # cannot license a match.
    gt, pt = set(gold.split()), set(pred.split())
    if len(gt) >= 2 and gt <= pt:
        return True
    if not lenient:
        return False
    if tok_f1(pred, gold) >= 0.6:
        return True
    # A prediction that CONTAINS the gold object is right but wordier:
    # "2 of the junior engineers" for "junior engineers", "proxmox host in the
    # auckland rack" for "auckland rack". Token-F1 punishes the extra words and
    # dipped just under threshold on several of these. Require the gold side to
    # be fully covered and non-trivial, so this does not license a match on a
    # single shared word.
    return False


SYMMETRIC = None  # populated from the ontology in main()
INVERSES = None

# Predicate equivalences for SCORING only.
#
# rel_type_canonicalize() folds synonyms onto seed relations, which is the
# production concern. This table covers the rest: pairs that are equally correct
# answers where neither side is a seed relation, or where the gold itself used a
# novel predicate. "speaks_language" is not a better or worse answer than
# "speaks" — scoring them as different costs a model both a false positive and a
# false negative for a naming choice that carries no information.
#
# Deliberately narrow: only genuine synonyms. studied_at is NOT equivalent to
# studied — "studied medicine" and "studied at Otago" are different facts.
EQUIV_PREDICATES = [
    {"speaks", "speaks_language", "speaks_fluently"},
    # Membership: joining, sitting on a board, attending, enrolling all assert the
    # same edge between the same two entities.
    {"attends", "member_of", "enrolled_at", "studies_at", "joined", "joined_at",
     "board_member", "started_at", "sits_on"},
    # Role/title.
    {"has_role", "profession", "job_title", "position", "role_at", "title"},
    # Acquaintance. "met" asserts the same durable edge as "knows".
    {"knows", "met", "acquainted_with"},
    # Locative. For one person or object these differ only in phrasing.
    {"lives_in", "located_in", "located_on", "located_at", "resides_in",
     "housed_in", "based_in"},
    {"mentors", "mentor_of", "is_mentor_of"},
    {"founded", "founder_of", "co_founded"},
    {"owns", "owner_of"},
    {"drives", "driver_of"},
    {"grew_up_in", "raised_in"},
    {"has_hostname", "hostname_is"},
]

# Converse predicates: same fact, endpoints swapped. The seed ontology declares
# inverses for kinship (parent_of/child_of) but not for predicates a model
# invents, and "X decided Y" is the converse of "Y decided_by X" whether or not
# the ontology says so. Six models produced the converse form of gv05 and all six
# were marked wrong for it.
SCORING_CONVERSES = {
    "decided": "decided_by",
    "decided_by": "decided",
    "supersedes": "superseded_by",
    "superseded_by": "supersedes",
    "employs": "works_for",
    "works_for": "employs",
    # Converses of MINTED predicates, which the ontology cannot declare because
    # they are not seed types. Measured on the 10k run: 75 predictions stated
    # `owner_of` with the arguments reversed against gold's `owns_account`, and
    # 27 stated `owns` reversed against `decided_by`. Those are the same fact
    # said the other way round, and scoring them wrong measures direction of
    # phrasing rather than extraction.
    #
    # NOT added: supersedes reversed against itself. v2 superseding v1 is not v1
    # superseding v2 — 29 predictions did exactly that and they are genuinely
    # wrong. Symmetry is a property of specific predicates, not a blanket excuse
    # for argument order.
    "owns_account": "owner_of",
    "owner_of": "owns_account",
    "owns_account_of": "owns_account",
    "manages_account": "account_managed_by",
    "account_managed_by": "manages_account",
    "owns": "owned_by",
    "owned_by": "owns",
    "customer_of": "supplies",
    "supplies": "customer_of",
    "member_of": "has_member",
    "has_member": "member_of",
    "located_in": "contains",
    "contains": "located_in",
}
_EQUIV = {}
for _grp in EQUIV_PREDICATES:
    for _r in _grp:
        _EQUIV[_r] = _grp


def rel_equal(a, b):
    return a == b or (a in _EQUIV and b in _EQUIV.get(a, ()))


USE_ALT = True


def triple_eq(p, g, lenient):
    # A gold triple may list alternative renderings of the same fact — naming a
    # device by description rather than hostname, located_in for a person where
    # lives_in was labelled. Matching an alternative satisfies the gold triple
    # and scores as a true positive; it is an equally correct answer, not one to
    # be excused from the denominator.
    if USE_ALT:
        for a in g.get("alt") or ():
            if _triple_eq_one(p, a, lenient):
                return True
    return _triple_eq_one(p, g, lenient)


def _triple_eq_one(p, g, lenient):
    if rel_equal(p["relation"], g["relation"]):
        if p["subject"] == g["subject"] and obj_match(p["object"], g["object"], lenient):
            return True
        # The ontology declares some relations symmetric ("one assertion implies
        # both directions"), so argument order carries no information for them.
        if g["relation"] in (SYMMETRIC or ()):
            return entity_match(p["subject"], g["object"], lenient) \
                and obj_match(p["object"], g["subject"], lenient)
        return False
    # inverse_rel_type is documented as "auto-enforced": asserting (a parent_of b)
    # commits (b child_of a) too, so the two forms are one fact and scoring them
    # as different answers measures direction of phrasing, not correctness.
    if (INVERSES or {}).get(g["relation"]) == p["relation"] \
       or SCORING_CONVERSES.get(g["relation"]) == p["relation"]:
        return entity_match(p["subject"], g["object"], lenient) \
            and obj_match(p["object"], g["subject"], lenient)
    return False


def match_note(preds, golds, lenient, ignore_relation=False):
    """Greedy 1-1 match. Returns (tp, matched_pred_idx).

    ignore_relation credits a pair on subject and object alone. That is not a
    quality metric — it is a diagnostic that separates "did not find the fact"
    from "found it and labelled the edge differently". The two failures have
    completely different fixes: the first needs a better model, the second is
    what the rel_types reconciliation gate already exists to absorb.

    GREEDY IS NOT OPTIMAL IN GENERAL, and that is a measured limitation rather
    than an assumption. If one prediction can satisfy two DIFFERENT gold triples
    in a note, greedy may consume it on the first and starve the second, scoring
    1 where maximum bipartite matching scores 2. Constructed case: gold
    `purchased` carrying alt `licenses`, alongside a second gold `licenses`, with
    both predicted — greedy returns tp=1, optimal returns tp=2.

    Checked against an augmenting-path implementation over the whole 10k v3 run:
    greedy 4118 true positives, optimal 4118, differing on ZERO notes. The
    pathological shape needs a prediction matching multiple DISTINCT gold triples
    in one note, and the alternates here cannot create it — an alt varies the
    relation while holding subject and object, so it collides with its own gold
    triple rather than with a sibling.

    Kept greedy because it is simple and changes nothing on this corpus. If a
    future corpus adds alternates that overlap ACROSS gold triples within a note,
    re-measure before trusting this.
    """
    used, tp = set(), 0
    for g in golds:
        for i, p in enumerate(preds):
            if i in used:
                continue
            if ignore_relation:
                ok = (p["subject"] == g["subject"]
                      and obj_match(p["object"], g["object"], lenient)) or \
                     (p["subject"] == g["object"]
                      and obj_match(p["object"], g["subject"], lenient))
            else:
                ok = triple_eq(p, g, lenient)
            if ok:
                used.add(i)
                tp += 1
                break
    return tp, used


FIRST_PERSON = {"user", "i", "me", "my", "myself", "we", "us"}


def ground_text(s):
    """Normalise for grounding comparisons.

    Models write kb_server for "KB server" and 7 for "seven"; both are the same
    entity written differently, and counting them as fabrication would measure
    spelling. Underscores and hyphens become spaces, and number words are mapped
    to digits on BOTH sides so the two forms meet.
    """
    if s is None:
        return ""
    s = re.sub(r"[_\-/]+", " ", str(s).casefold())
    s = re.sub(r"[^\w\s.:]+", " ", s)
    s = re.sub(r"\s+", " ", s).strip()
    # Dots are kept so IPs and hostnames survive, but a sentence-final "seven."
    # must still meet the digit form, so strip trailing dots per token.
    return " ".join(NUMBER_WORDS.get(t.rstrip("."), t.rstrip("."))
                    for t in s.split())


def grounded(value, note_norm):
    """Can this argument be traced to the source note?

    Deliberately independent of the gold labels: it asks whether the model
    invented an entity, not whether it picked the entity I happened to label.
    A fabricated endpoint is the failure that matters most for a drain writing
    into memory_facts, because the write gate cannot catch it — a well-formed
    triple about a person who was never mentioned looks exactly like a good one.

    "user" is grounded by convention: the prompt instructs the model to use it as
    the subject for first-person notes.
    """
    v = ground_text(value)
    if not v or v in FIRST_PERSON:
        return True
    if v in note_norm:
        return True
    note_toks = set(note_norm.split())
    toks = [t for t in v.split() if t not in ARTICLES and len(t) > 2]
    if not toks:
        return v in note_toks
    hit = sum(1 for t in toks if t in note_toks or t in note_norm)
    # Majority of content tokens present: tolerates "Rakuen Software Ltd" for
    # "Rakuen Software" without tolerating an invented name.
    return hit * 2 >= len(toks)


def prf(tp, fp, fn):
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    f = 2 * p * r / (p + r) if p + r else 0.0
    return p, r, f


def derive_schema_ok(row):
    """Re-derive schema validity from the raw output, correcting the runners.

    The runners recorded schema_ok=False for anything without a "facts" array,
    which swept up {} and [] — and every single one of those, across every model,
    turned out to be an empty answer on a note that asserts no durable fact. That
    is a correct abstention in a terser shape, not a malformed response, and
    production agrees: mf_commit_facts() commits nothing either way.

    Counting it as a schema failure produced a false headline — schema validity
    appearing to degrade monotonically with model size (1.00 -> 0.96 -> 0.84 ->
    0.77) when what actually varies is how tersely a model says "nothing here".
    It also excluded those notes from the abstention denominator, understating
    abstention for exactly the models that abstained most.

    schema_ok is now False only for output that carries content in the wrong
    shape (a bare fact object, prose, unparseable text) — a real failure that
    silently commits nothing.
    """
    raw = (row.get("raw") or "").strip()
    if raw in ("{}", "[]", "{ }", "[ ]", ""):
        return True
    start, end = raw.find("{"), raw.rfind("}")
    if start == -1 or end < start:
        return False
    try:
        obj = json.loads(raw[start:end + 1])
    except json.JSONDecodeError:
        return False
    if isinstance(obj, dict) and isinstance(obj.get("facts"), list):
        return True
    # Valid JSON, no facts array, and not empty: wrong shape carrying content.
    return isinstance(obj, dict) and not obj


def load_pred_file(path, gold_rows):
    """Load a prediction file, refusing an incomplete one.

    The completeness check used to live only in main(), so every ad-hoc analysis
    that imported this module skipped it — and a partial run scores as
    catastrophically bad rather than as missing. A 1-note remnant once reported
    F1 0.031 for a model that scores 0.926, and a half-finished 31B run reported
    0.527 in an order-invariance check. Shared loader so that cannot recur.
    """
    rows = [json.loads(l) for l in open(path) if l.strip()]
    excluded = {r["id"] for r in gold_rows if r.get("excluded")}
    rows = [r for r in rows if r["id"] not in excluded]
    wanted = [r for r in gold_rows if r["id"] not in excluded]
    if len(rows) != len(wanted):
        raise ValueError(
            f"incomplete predictions: {path} has {len(rows)} scorable rows, "
            f"gold has {len(wanted)}. The run is unfinished or was abandoned.")
    return rows


def load_gold_file(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def load_triples(rows, key, canonicalize=False):
    """canonicalize applies rel_type_canonicalize()'s alias folding, which is what
    the commit path now does before a triple reaches the gate. Applied to
    predictions only — gold labels are authored canonical."""
    out = {}
    for r in rows:
        ts = []
        for t in r.get(key) or []:
            rel = t.get("relation")
            if canonicalize:
                rel = prompt.canonicalize_relation(rel)
            entry = {
                "subject": norm_entity(t.get("subject")),
                "relation": norm(rel),
                "object": norm_entity(t.get("object")),
            }
            if t.get("alt"):
                entry["alt"] = [{
                    "subject": norm_entity(a.get("subject")),
                    "relation": norm(a.get("relation")),
                    "object": norm_entity(a.get("object")),
                } for a in t["alt"]]
            ts.append(entry)
        out[r["id"]] = ts
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--pred", required=True)
    ap.add_argument("--pred-key", default="pred_grounded",
                    help="'pred_grounded' (default) scores what production commits "
                         "today: every extracted fact whose subject and object both "
                         "trace to the note. 'pred' applies the retired "
                         "MF_CONF_FLOOR instead, and 'pred_nofloor' applies no gate "
                         "at all. See the note in main() on why the default moved.")
    ap.add_argument("--no-alt", action="store_true",
                    help="ignore alternative renderings entirely: both endpoints "
                         "must match the labelled entity exactly. The strictest "
                         "reading available.")
    ap.add_argument("--allow-thinking-off", action="store_true",
                    help="score a run that recorded thinking:true but produced no "
                         "reasoning on any row. Only for re-deriving the score of a "
                         "known-suppressed historical run (see defect 31); it is "
                         "not a valid measurement of the model.")
    ap.add_argument("--no-alias", action="store_true",
                    help="skip rel_type_canonicalize() alias folding. Production "
                         "folds, so this only exists to measure what aliasing buys.")
    ap.add_argument("--json-out")
    args = ap.parse_args()

    gold_rows = [json.loads(l) for l in open(args.gold) if l.strip()]
    pred_rows = [json.loads(l) for l in open(args.pred) if l.strip()]
    # Items flagged excluded are defective as benchmark questions, not merely
    # hard. Predictions for them are dropped from both numerator and denominator.
    excluded = {r["id"] for r in gold_rows if r.get("excluded")}
    if excluded:
        gold_rows = [r for r in gold_rows if r["id"] not in excluded]
        pred_rows = [r for r in pred_rows if r["id"] not in excluded]
    # An abandoned run leaves a short prediction file behind, and a partial file
    # scores as catastrophically bad rather than as missing — a 1-note remnant of
    # a killed run once surfaced as F1 0.031 for a model that actually scores
    # 0.853. Refuse rather than report a number that looks real.
    if len(pred_rows) != len(gold_rows):
        raise SystemExit(
            f"incomplete predictions: {args.pred} has {len(pred_rows)} rows, "
            f"gold has {len(gold_rows)}. Re-run the model or delete the partial file.")
    # Refuse any row the harness stopped from producing a response.
    #
    # This is the third place the same defect has appeared. score_b.py learned it
    # twice: once for --max-tokens (gemma-4-12B lost 0.17 coverage to one
    # truncated row) and once for --timeout (Qwen3.6-27B scored 0.50/0.40 on
    # three timeouts). Tier-A never got the check, and it cost the largest wrong
    # claim in this whole effort.
    #
    # sweep_thinking.sh caps completions at 2048 where production allows
    # MF_LLM_OUT_CAP (8192). Models that reason at length blow through it and
    # emit NOTHING: 11 of 70 notes for gemma-4-26B-A4B, 8 for gemma-4-12B, 0 for
    # E4B and E2B. Those empty rows scored as abstentions AND as missed facts, so
    # abstention rose 0.78 -> 0.96 and recall fell 0.94 -> 0.84, and I reported
    # that as "thinking hurts the bigger model". It was the cap, and the cap was
    # mine.
    #
    # The header of sweep_thinking.sh anticipated exactly this: "If that recurs
    # here it should show as truncation, not as a mystery." It recorded the
    # field. Nothing read it.
    # Errors block too, not just truncation. score_b.py learned this when
    # Qwen3.6-27B timed out on three topics and scored 0.50/0.40; score.py was
    # given the truncation half of that lesson and not the other half, and then
    # Magistral-Small-2509 recorded RemoteDisconnected on all 70 notes because I
    # killed its server mid-run. Without this the run scores as a model that
    # emits nothing.
    errs = {r["id"]: r["error"] for r in pred_rows if r.get("error")}
    if errs:
        sample = ", ".join(f"{k} ({v})" for k, v in list(errs.items())[:3])
        raise SystemExit(
            f"harness-blocked predictions: {args.pred} has {len(errs)} errored "
            f"rows: {sample}{' ...' if len(errs) > 3 else ''}. The transport "
            f"failed, so these say nothing about the model. Re-run them.")
    cut = [r["id"] for r in pred_rows if r.get("truncated")]
    if cut:
        raise SystemExit(
            f"harness-blocked predictions: {args.pred} rows {cut} hit the runner's "
            f"--max-tokens. Production allows MF_LLM_OUT_CAP (8192); re-run those "
            f"with a cap at least that high. Scoring them charges the model for a "
            f"bound I chose, and an empty response is indistinguishable from "
            f"abstention once it reaches the scorer.")
    # Refuse a run that asked for reasoning and did not get any.
    #
    # This is the FOURTH instance of the defect the two blocks above describe, and
    # the comment there names it exactly: "It recorded the field. Nothing read it."
    # run_llamacpp.py has written `reasoning_chars` on every row since the GLM
    # triage. Nothing has ever read it. So the v4 10k E4B run wrote
    #
    #     {"thinking": true, "reasoning_chars": 0}
    #
    # ten thousand times -- a row contradicting itself -- and scored 0.5947 without
    # complaint, because the scorer compares triples to gold and has no notion of
    # how they were produced. gemma-4-E4B was suppressing its own reasoning in
    # response to the prompt's "No prose, no markdown." (defect 31), which costs it
    # 0.084 F1 and is invisible in every other field: valid JSON, clean parse,
    # nothing truncated, no error.
    #
    # Fires only when the rows CLAIM thinking. A --no-thinking ablation records
    # thinking:false and passes, which is the correct distinction: the fault is not
    # "no reasoning", it is "asked for reasoning, recorded none, reported anyway".
    want = [r for r in pred_rows if r.get("thinking")]
    if want and not any((r.get("reasoning_chars") or 0) > 0 for r in want):
        if not args.allow_thinking_off:
            raise SystemExit(
                f"harness-blocked predictions: {args.pred} has {len(want)} rows "
                f"with thinking:true and not one with any reasoning. The run asked "
                f"for reasoning and the model emitted none, so this scores a "
                f"configuration nobody chose -- see defect 31. Re-run it, or pass "
                f"--allow-thinking-off if you are deliberately scoring a "
                f"known-suppressed run for the record.")
        print(f"WARNING: {args.pred} recorded thinking:true with zero reasoning on "
              f"all {len(want)} rows; scoring anyway at your request.")

    # The default scoring view is the gate the product actually applies.
    #
    # It used to be 'pred', the MF_CONF_FLOOR view, and that stayed the default
    # after the floor was removed from src/kb/kb_memory_facts.c and replaced by
    # fact_grounded(). So every Tier-A number was scored against a gate the
    # shipping code no longer has. The cost was not uniform: it took 0.006 off
    # gemma-4-E4B and it took ALL of Qwen3-0.6B, which extracted 72 triples,
    # every one of them discarded at the floor, and was reported as F1 0.0000.
    # The mechanism was already written down in run_hf.py's docstring: the
    # prompt's own schema example carries the literal "confidence":0.0 and small
    # models copy it. The floor measured prompt-copying, not extraction.
    #
    # Synthesised here rather than in the runners so every prediction file
    # already on disk gets the corrected view without being re-run.
    if args.pred_key == "pred_grounded":
        gnote = {r["id"]: ground_text(r["note"]) for r in gold_rows}
        for r in pred_rows:
            nn = gnote[r["id"]]
            r["pred_grounded"] = [
                t for t in (r.get("pred_nofloor") or [])
                if grounded(t.get("subject"), nn) and grounded(t.get("object"), nn)
            ]
    gold = load_triples(gold_rows, "gold")
    pred = load_triples(pred_rows, args.pred_key, canonicalize=not args.no_alias)
    cat = {r["id"]: r["category"] for r in gold_rows}
    pmeta = {r["id"]: r for r in pred_rows}
    for r in pred_rows:
        r["schema_ok"] = derive_schema_ok(r)

    global USE_ALT
    USE_ALT = not args.no_alt
    seed = set(prompt.seed_relations())
    global SYMMETRIC, INVERSES
    SYMMETRIC = prompt.symmetric_relations()
    INVERSES = prompt.inverse_relations()

    # Diagnostic: how much of the error is edge-labelling rather than a missed
    # fact? Scored on the lenient object rule, relation ignored.
    TPr = FPr = FNr = 0
    for nid, g in gold.items():
        p = pred.get(nid, [])
        tp, used = match_note(p, g, True, ignore_relation=True)
        TPr, FPr, FNr = TPr + tp, FPr + (len(p) - len(used)), FNr + (len(g) - tp)
    Pr, Rr, Fr = prf(TPr, FPr, FNr)

    report = {}
    for mode in ("strict", "lenient"):
        lenient = mode == "lenient"
        TP = FP = FN = 0
        by_cat = defaultdict(lambda: [0, 0, 0])
        for nid, g in gold.items():
            p = pred.get(nid, [])
            tp, used = match_note(p, g, lenient)
            fp, fn = len(p) - len(used), len(g) - tp
            TP, FP, FN = TP + tp, FP + fp, FN + fn
            c = by_cat[cat[nid]]
            c[0] += tp; c[1] += fp; c[2] += fn
        P, R, F = prf(TP, FP, FN)
        report[mode] = {
            "precision": round(P, 4), "recall": round(R, 4), "f1": round(F, 4),
            "tp": TP, "fp": FP, "fn": FN,
            # A category whose notes carry no gold triples (transient, most of
            # ambiguous and negation) has undefined P/R/F1. Reporting 0.0 there
            # inverts the meaning: fp=0 on a factless note is perfect restraint,
            # not failure, and a chart would show it as the worst category.
            # Emit null and read abstention_rate_on_schema for those instead.
            "by_category": {k: (dict(zip(("precision", "recall", "f1"),
                                         [round(x, 4) for x in prf(*v)]),
                                     tp=v[0], fp=v[1], fn=v[2])
                                if (v[0] + v[2]) > 0 else
                                {"precision": None, "recall": None, "f1": None,
                                 "tp": v[0], "fp": v[1], "fn": v[2],
                                 "_note": "no gold triples in this category; "
                                          "see abstention_rate_on_schema"})
                            for k, v in sorted(by_cat.items())},
        }

    # Fabrication: triples with an endpoint that cannot be traced to the note.
    # Reported separately from precision because the two failures differ in kind
    # — a mislabelled edge is recoverable downstream, an invented entity is not.
    notes = {r["id"]: ground_text(r["note"]) for r in gold_rows}
    ungrounded, ungrounded_examples, total_pred = 0, [], 0
    for nid, ts in pred.items():
        nn = notes.get(nid, "")
        for t in ts:
            total_pred += 1
            bad = [k for k in ("subject", "object") if not grounded(t[k], nn)]
            if bad:
                ungrounded += 1
                if len(ungrounded_examples) < 12:
                    ungrounded_examples.append(
                        {"id": nid, "triple": [t["subject"], t["relation"], t["object"]],
                         "ungrounded_args": bad})
    report["fabrication"] = {
        "_note": "a triple is counted here when a subject or object cannot be traced "
                 "to the source note. Gold-independent: it measures invented entities, "
                 "not disagreement with my labels. The write gate cannot catch these — "
                 "a well-formed triple about someone never mentioned looks valid.",
        "predicted_triples": total_pred,
        "ungrounded_triples": ungrounded,
        "fabrication_rate": round(ungrounded / total_pred, 4) if total_pred else None,
        "examples": ungrounded_examples,
    }

    report["relation_agnostic"] = {
        "_note": "diagnostic, not a quality score: subject+object matched, relation "
                 "ignored. The gap to lenient F1 is the share of error that is edge "
                 "labelling rather than a missed fact.",
        "precision": round(Pr, 4), "recall": round(Rr, 4), "f1": round(Fr, 4),
        "tp": TPr, "fp": FPr, "fn": FNr,
    }

    # Over-extraction: notes whose gold is the empty list. Any triple here is a
    # false positive that the write gate would then have to catch.
    #
    # Abstention is counted only over notes where the model actually emitted the
    # {"facts":[...]} shape. A model that returns valid JSON of the wrong shape
    # commits nothing in production, but it has not decided the note is factless —
    # crediting that as abstention would make a broken model look maximally
    # precise. Both denominators are reported so the distinction stays visible.
    empty_ids = [i for i, g in gold.items() if not g]
    spurious = sum(len(pred.get(i, [])) for i in empty_ids)
    on_schema = [i for i in empty_ids if pmeta.get(i, {}).get("schema_ok")]
    # Note: an explicit {} on a factless note now counts as a schema-valid
    # abstention, so these denominators include it.
    clean = sum(1 for i in on_schema if not pred.get(i))
    report["over_extraction"] = {
        "empty_gold_notes": len(empty_ids),
        "on_schema_empty_gold_notes": len(on_schema),
        "notes_correctly_empty": clean,
        "abstention_rate_on_schema": round(clean / len(on_schema), 4) if on_schema else None,
        "spurious_triples": spurious,
    }

    # Operational health: did we get parseable JSON of the right shape, and did it
    # respect the ontology?
    ok = sum(1 for r in pred_rows if r.get("parse_ok"))
    schema = sum(1 for r in pred_rows if r["schema_ok"])
    rels = [t["relation"] for ts in pred.values() for t in ts]
    report["output_health"] = {
        "notes": len(pred_rows),
        "json_parse_ok": ok,
        "json_parse_rate": round(ok / len(pred_rows), 4) if pred_rows else 0.0,
        "schema_ok": schema,
        "schema_rate": round(schema / len(pred_rows), 4) if pred_rows else 0.0,
        "malformed_facts": sum(r.get("malformed_facts", 0) for r in pred_rows),
        "dropped_by_conf_floor": sum(r.get("dropped_by_conf_floor", 0) for r in pred_rows),
        "predicted_triples": len(rels),
        "in_seed_ontology": round(sum(1 for r in rels if r in seed) / len(rels), 4) if rels else None,
        "catch_all_relations": sum(1 for r in rels if r in {"other", "unknown", "misc"}),
    }
    lat = sorted(r["latency_ms"] for r in pred_rows if r.get("latency_ms") is not None)
    if lat:
        report["latency_ms"] = {
            "median": lat[len(lat) // 2],
            "p90": lat[int(len(lat) * 0.9)],
            "mean": round(sum(lat) / len(lat), 1),
        }
    toks = [r["completion_tokens"] for r in pred_rows if r.get("completion_tokens") is not None]
    if toks:
        report["completion_tokens"] = {
            "median": sorted(toks)[len(toks) // 2],
            "mean": round(sum(toks) / len(toks), 1),
            "max": max(toks),
        }
    report["model"] = pmeta[pred_rows[0]["id"]].get("model") if pred_rows else None
    report["scored_key"] = args.pred_key
    report["alias_folding"] = not args.no_alias

    out = json.dumps(report, indent=2)
    print(out)
    if args.json_out:
        open(args.json_out, "w").write(out + "\n")


if __name__ == "__main__":
    main()
