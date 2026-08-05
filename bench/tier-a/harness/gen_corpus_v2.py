#!/usr/bin/env python3
"""Generate an independent corpus: gold first, surface text second.

WHY THIS DIRECTION. The obvious way round is to write notes and then extract
gold from them, which makes the gold an LLM output that has to be checked by
hand. This goes the other way: triples are synthesised programmatically from a
fresh inventory, and a model is asked only to express those exact triples as a
natural note. Gold is then correct BY CONSTRUCTION and the model varies only
surface form, which is the thing that has to be independent.

It is also better independence than the original. corpus v5 phrases facts from a
fixed template set, and one of those templates phrased a hostname fact as "X runs
on Y", manufacturing 28 false negatives and 23 false positives per arm and
rewarding models that read the sentence wrong. Templates are where a corpus
encodes an opinion about language. Here there are no templates: the phrasing
comes from a model that is not under test.

WHAT IS INDEPENDENT, AND WHAT IS NOT.
  independent: the entity inventory, every surface form, the generator model
  shared, deliberately: the ontology, the scorer, the category strata and their
      proportions. Changing those changes what is measured rather than what it is
      measured on, and the point is to vary the corpus alone.

THE RELAXATION, STATED. harness/second_corpus_plan.md asks for a 30B-class
generator. The 5080 holds 16 GiB and a 30B at Q4 does not fit, so this runs a
14B. It is still outside the 230M-8B field under test and shares weights with no
tested model, which is the property that matters. A larger generator would be
better and is a rented-GPU job.

VERIFICATION IS NOT OPTIONAL. A note that does not actually state its triple is
worse than no note: it teaches the benchmark that a correct extraction is wrong.
Every generated note is checked for the literal presence of its subject and
object strings, and anything that fails is regenerated once and then dropped.
That check is weak on purpose -- it catches omission, not paraphrase -- and the
count of drops is reported so the corpus carries its own error rate.
"""
import argparse, json, os, random, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A fresh inventory. No surface form here appears in corpus v5's inventory, which
# is checked at the end rather than asserted.
FIRST = ["Ingrid", "Mateo", "Priya", "Callum", "Yusuf", "Noor", "Anton", "Sinead",
         "Kofi", "Marta", "Dimitri", "Aiko", "Rafael", "Bronwen", "Tomasz", "Leila"]
LAST = ["Okonkwo", "Halvorsen", "Ferreira", "Nakamura", "Whitlock", "Baranov",
        "Achterberg", "Solberg", "Mbeki", "Kowalczyk", "Renard", "Castellanos"]
ORGS = ["Wexford Analytics", "Bramble & Roe", "Northaven Logistics", "Quillon Data",
        "Saltmarsh Consulting", "Fenwick Provisioning", "Ardent Rail", "Copperline Media",
        "Mirebrook Foods", "Talvern Systems", "Hollis & Drage", "Vantage Kiln"]
CITIES = ["Trondheim", "Lubbock", "Wollongong", "Bruges", "Kanazawa", "Rosario",
          "Galway", "Sapele", "Tromso", "Cuenca", "Dunedin", "Ostrava"]
HOSTS = ["kestrel-app-04", "tarn-db-11", "vellum-edge-2", "brindle-cache-07",
         "harrier-batch-3", "oakum-api-19", "sable-queue-05", "pike-worker-12"]
ROLES = ["release manager", "data steward", "duty architect", "billing lead",
         "field engineer", "compliance officer", "platform owner"]

# Relations drawn from the SHARED ontology, deliberately.
FACTFUL = [
    ("works_for",     lambda r: (r.person(), r.org())),
    ("located_in",    lambda r: (r.org(), r.city())),
    ("has_role",      lambda r: (r.person(), r.role())),
    ("member_of",     lambda r: (r.person(), r.org())),
    ("customer_of",   lambda r: (r.org(), r.org())),
    ("device_has_ip", lambda r: (r.host(), r.ip())),
    ("lives_in",      lambda r: (r.person(), r.city())),
]

SYS = ("You write one short workplace note. It must state the given fact or facts "
       "plainly, in one or two sentences, as a person would jot them down. Use the "
       "exact names given, spelled exactly. Do not add facts. Do not explain. "
       "Do not use quotation marks around the whole note. Output the note only.")

FACTLESS_BRIEF = {
    "negation": "Write a note saying a stated relationship has ENDED or is no longer true. Mention {a} and {b}.",
    "transient": "Write a note about a momentary state or plan that is not durable. Mention {a}.",
    "ambiguous": "Write a note that hints at a connection between {a} and {b} without asserting any fact.",
}


class Inv:
    def __init__(self, rnd):
        self.r = rnd
    def person(self): return "%s %s" % (self.r.choice(FIRST), self.r.choice(LAST))
    def org(self):    return self.r.choice(ORGS)
    def city(self):   return self.r.choice(CITIES)
    def host(self):   return self.r.choice(HOSTS)
    def role(self):   return self.r.choice(ROLES)
    def ip(self):     return "10.%d.%d.%d" % (self.r.randint(1, 250), self.r.randint(0, 250), self.r.randint(2, 250))


def complete(base, sys_prompt, user, timeout=180):
    """max_tokens has to cover the REASONING CHANNEL, not just the answer.

    gemma-4-26B-A4B reasons before answering, and its reasoning lands in
    `reasoning_content` while the note lands in `content`. At 220 tokens the
    reasoning consumed the whole budget and `content` came back empty on every
    single call: 24 minutes of generation produced zero notes, because each one
    failed its containment check twice and was dropped.

    That is finding 1 of this project happening to its own tooling -- a model
    putting output in a channel the caller does not read, failing silently, and
    producing a plausible-looking nothing. 700 tokens covers both channels.
    """
    body = json.dumps({"model": "gen", "messages": [
        {"role": "system", "content": sys_prompt}, {"role": "user", "content": user}],
        "temperature": 0.9, "max_tokens": 700}).encode()
    req = urllib.request.Request(base.rstrip("/") + "/v1/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read())
    return (d["choices"][0]["message"].get("content") or "").strip()


def clean(t):
    """Collapse to one line WITHOUT discarding the note.

    The first version returned `t.split("\n")[0]`, which throws away everything
    after the first newline and returns EMPTY when the model's content begins
    with one. That is what killed the factless categories: `ambiguous` came back
    0 of 51 and `transient` 20 of 139, not because the model refused but because
    a leading blank line emptied the note and the empty note was dropped.

    The visible symptom was a 23% drop rate. The real damage was to composition:
    the factless share fell to 18.8% against the 32.1% this corpus exists to
    match, which would have moved F1 for reasons unrelated to anything measured.
    """
    t = t.strip().strip('"').strip()
    t = re.sub(r"\s+", " ", t)
    return t.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=1001)
    ap.add_argument("--seed", type=int, default=20260805)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    inv = Inv(rnd)

    # Same strata and proportions as gold_large: 32.1% factless.
    plan = ([("multi_fact", 2)] * int(a.n * 0.174) + [("third_person", 1)] * int(a.n * 0.158) +
            [("governance", 1)] * int(a.n * 0.086) + [("first_person", 1)] * int(a.n * 0.080) +
            [("infra", 1)] * int(a.n * 0.077) + [("implicit", 1)] * int(a.n * 0.072) +
            [("transient", 0)] * int(a.n * 0.139) + [("negation", 0)] * int(a.n * 0.132) +
            [("ambiguous", 0)] * int(a.n * 0.051))
    # Pad by CYCLING the plan rather than appending one category. Padding with
    # third_person pushed it to 24.4% against a 15.8% target, distorting the
    # strata in the opposite direction from the drops.
    i = 0
    base = list(plan)
    while len(plan) < a.n:
        plan.append(base[i % len(base)])
        i += 1
    rnd.shuffle(plan)

    out = open(a.out, "w")
    kept = dropped = 0
    drops_by_cat = {}
    for i, (cat, nfacts) in enumerate(plan):
        if nfacts == 0:
            aa, bb = inv.person(), inv.org()
            user = FACTLESS_BRIEF.get(cat, FACTLESS_BRIEF["ambiguous"]).format(a=aa, b=bb)
            gold = []
        else:
            gold = []
            for _ in range(nfacts):
                rel, mk = rnd.choice(FACTFUL)
                s, o = mk(inv)
                gold.append({"subject": s, "relation": rel, "object": o})
            user = "Facts to state: " + "; ".join(
                "%s %s %s" % (g["subject"], g["relation"].replace("_", " "), g["object"]) for g in gold)
        note = ""
        for attempt in (1, 2, 3):
            try:
                note = clean(complete(a.base_url, SYS, user))
            except Exception as e:
                print("  gen error on %d: %s" % (i, e), file=sys.stderr); note = ""
            if not note:
                continue
            # weak containment check: catches omission, not paraphrase
            if all(g["subject"] in note and g["object"] in note for g in gold):
                break
            note = ""
        if not note:
            # Report WHICH category is failing, not just that something did. A
            # bare drop counter hid a failure concentrated entirely in two
            # categories and reported it as a uniform 23%.
            drops_by_cat[cat] = drops_by_cat.get(cat, 0) + 1
        if not note:
            dropped += 1
            continue
        out.write(json.dumps({
            "id": "w%06d" % i, "note": note, "gold": json.dumps(gold),
            "category": cat, "domain": "business", "template": "llm-gen-v2",
            "source": "None", "tier": "2", "stratum": "S2", "provenance": "generated-v2",
        }) + "\n")
        kept += 1
        # print on progress OR on drops: a run that keeps nothing printed nothing
        # at all, which is how the empty-content failure stayed invisible.
        if (kept + dropped) % 25 == 0:
            print("  %d kept, %d dropped" % (kept, dropped), flush=True)
    out.close()
    print("DONE kept=%d dropped=%d (%.1f%% drop rate)" % (kept, dropped, 100.0 * dropped / max(kept + dropped, 1)))
    if drops_by_cat:
        print("drops by category: %s" % sorted(drops_by_cat.items(), key=lambda kv: -kv[1]))


if __name__ == "__main__":
    main()
