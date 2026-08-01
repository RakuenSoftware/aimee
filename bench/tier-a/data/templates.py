"""Template bank: (domain, category) -> many surface forms.

Step 3 of HARNESS_DESIGN.md, and the part that decides whether a 10,000-note
corpus is worth more than the 70 hand-written notes it replaces.

THE FAILURE THIS FILE EXISTS TO PREVENT. A generated corpus can be EASIER than a
hand-written one. If every first-person note is "I work for {ORG}", a model
learns the shape rather than the task, every score rises, the ladder compresses,
and the benchmark quietly stops discriminating — while looking more authoritative
because n is larger. Guarding against that is a numbers game: many templates per
cell, large entity inventories so surface form varies independently of template,
and clause-order variation within templates.

CONTRACT FOR A TEMPLATE.

    text   a format string over the fact's fields.
    gold   a list of {s, r, o} format strings, or [] for the empty-gold
           categories (transient, most negation, most ambiguous).

Gold is written ALONGSIDE the text and from the same fields, so the triple cannot
disagree with the sentence — that is what "correct by construction" means here.
A template whose text mentions an entity its gold does not is a bug, and
check_diversity.py has a groundedness check for exactly that.

EMPTY-GOLD CATEGORIES ARE NOT FILLER. Roughly a third of the corpus asserts no
durable fact, because over-extraction is the expensive failure: the drain commits
into memory_facts and the write gate promotes to durable edges, so a model that
invents a triple for "the call went well" poisons the graph. Those notes carry
the precision signal and must sound like real non-durable statements rather than
caricatures.
"""

# ---------------------------------------------------------------- code domain
# Facts here come from git and are verifiable. Field names match the inventory
# emitted by mine_entities.py.

CODE = {
    "governance": [
        # kind == "rename": the basename genuinely changed, so also_known_as holds.
        {"fact": "rename",
         "text": "{old_base} was renamed to {new_base} in {repo}.",
         "gold": [{"s": "{old_base}", "r": "also_known_as", "o": "{new_base}"}]},
        {"fact": "rename",
         "text": "In {repo} we now call {old_base} {new_base}.",
         "gold": [{"s": "{old_base}", "r": "also_known_as", "o": "{new_base}"}]},
        {"fact": "rename",
         "text": "{new_base} is the new name for {old_base}.",
         "gold": [{"s": "{old_base}", "r": "also_known_as", "o": "{new_base}"}]},
        {"fact": "version",
         "text": "{new_ver} supersedes {old_ver} in {repo}.",
         "gold": [{"s": "{new_ver}", "r": "supersedes", "o": "{old_ver}"}]},
        {"fact": "version",
         "text": "{repo} moved from {old_ver} to {new_ver}.",
         "gold": [{"s": "{new_ver}", "r": "supersedes", "o": "{old_ver}"}]},
        {"fact": "version",
         "text": "We cut {new_ver} of {repo}, replacing {old_ver}.",
         "gold": [{"s": "{new_ver}", "r": "supersedes", "o": "{old_ver}"}]},
    ],
    "third_person": [
        # kind == "move": the name did NOT change, only the directory. Asserting
        # also_known_as here would claim a file is now called what it already
        # was — the bug that 1,609 of 1,758 detected path changes would have
        # introduced if moves and renames were not separated.
        {"fact": "move",
         "text": "{base} now lives in {new_dir} in {repo}.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "move",
         "text": "In {repo}, {base} sits under {new_dir} these days.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "authorship",
         "text": "{person} contributes to {repo}.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{repo}"}]},
        {"fact": "authorship",
         "text": "{person} is one of the {repo} maintainers.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{repo}"}]},
    ],
    "multi_fact": [
        {"fact": "move",
         "text": "{base} used to be in {old_dir}; it is under {new_dir} now.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "authorship2",
         "text": "{person} maintains {repo} and also has commits in {repo2}.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{repo}"},
                  {"s": "{person}", "r": "member_of", "o": "{repo2}"}]},
        {"fact": "move",
         "text": "{base} moved from {old_dir} to {new_dir} in {repo}.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        # More move-based shapes than authorship2 ones on purpose: the
        # authorship2 pool needs a person with >=3 commits in >=2 repos and is
        # small, so without these the two move templates carried ~50% of the
        # cell each and tripped the load gate on a data limit rather than a
        # generator fault.
        {"fact": "move",
         "text": "In {repo}, {base} was shifted out of {old_dir} into {new_dir}.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "move",
         "text": "{repo} keeps {base} in {new_dir} now, not {old_dir}.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "move",
         "text": "We relocated {base} to {new_dir} in {repo}.",
         "gold": [{"s": "{base}", "r": "located_in", "o": "{new_dir}"}]},
        {"fact": "authorship2",
         "text": "{person} works on both {repo} and {repo2}.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{repo}"},
                  {"s": "{person}", "r": "member_of", "o": "{repo2}"}]},
    ],
    "negation": [
        # A deleted path is verifiable, and the note asserts a removal, so there
        # is no durable fact to commit. Deliberately NOT phrased as a claim about
        # the concept: a file often moves repos rather than ceasing to exist.
        {"fact": "deletion",
         "text": "{base} was deleted from {repo}.",
         "gold": []},
        {"fact": "deletion",
         "text": "We dropped {base} from {repo} entirely.",
         "gold": []},
        {"fact": "deletion",
         "text": "{base} is gone from {repo} now.",
         "gold": []},
    ],
    "transient": [
        {"fact": "repo",
         "text": "The {repo} build is red again, third time today.",
         "gold": []},
        {"fact": "repo",
         "text": "Might pick the {repo} refactor back up next week.",
         "gold": []},
        {"fact": "repo",
         "text": "Spent the afternoon staring at {repo} CI logs.",
         "gold": []},
        {"fact": "repo",
         "text": "{repo} is being annoying today.",
         "gold": []},
    ],
    "infra": [
        {"fact": "host",
         "text": "{host} answers on {ip}.",
         "gold": [{"s": "{host}", "r": "device_has_ip", "o": "{ip}"}]},
        {"fact": "host",
         "text": "The {service} box has hostname {host} and IP {ip}.",
         "gold": [{"s": "{service}", "r": "has_hostname", "o": "{host}"},
                  {"s": "{service}", "r": "device_has_ip", "o": "{ip}"}]},
        {"fact": "host",
         "text": "{service} runs on {host}.",
         "gold": [{"s": "{service}", "r": "has_hostname", "o": "{host}"}]},
    ],
}

# ------------------------------------------------------ business / sales domain
# Entities are synthetic; the facts are decided before the sentence is written.

BUSINESS = {
    "first_person": [
        {"text": "I work for {company} now.",
         "gold": [{"s": "user", "r": "works_for", "o": "{company}"}]},
        {"text": "I'm the {role} at {company}.",
         "gold": [{"s": "user", "r": "has_role", "o": "{role}"},
                  {"s": "user", "r": "works_for", "o": "{company}"}]},
        {"text": "Joined {company} as {role} this month.",
         "gold": [{"s": "user", "r": "works_for", "o": "{company}"},
                  {"s": "user", "r": "has_role", "o": "{role}"}]},
        {"text": "I sit on the {team}.",
         "gold": [{"s": "user", "r": "member_of", "o": "{team}"}]},
    ],
    "third_person": [
        {"text": "{person} works for {company}.",
         "gold": [{"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "{person} is the {role} at {company}.",
         "gold": [{"s": "{person}", "r": "has_role", "o": "{role}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "{company} is based in {city}.",
         "gold": [{"s": "{company}", "r": "located_in", "o": "{city}"}]},
        {"text": "{person} joined the {team} last quarter.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{team}"}]},
    ],
    "multi_fact": [
        {"text": "{person} moved to the {team} at {company}.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{team}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "{company} is in {city}; {person} is the {role} there.",
         "gold": [{"s": "{company}", "r": "located_in", "o": "{city}"},
                  {"s": "{person}", "r": "has_role", "o": "{role}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "{person} is {role} at {company}, which is based in {city}.",
         "gold": [{"s": "{person}", "r": "has_role", "o": "{role}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"},
                  {"s": "{company}", "r": "located_in", "o": "{city}"}]},
        {"text": "Met {person} from {company} in {city}; they're the {role}.",
         "gold": [{"s": "user", "r": "knows", "o": "{person}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"},
                  {"s": "{person}", "r": "has_role", "o": "{role}"}]},
    ],
    "governance": [
        {"text": "The {policy} was decided by the {team}.",
         "gold": [{"s": "{policy}", "r": "decided_by", "o": "{team}"}]},
        {"text": "{team} owns the {policy}.",
         "gold": [{"s": "{policy}", "r": "decided_by", "o": "{team}"}]},
        # The gold names both policies in full, so the note must too. An
        # earlier version said "the {prev_year} one" and asserted a gold entity
        # that appears nowhere in the text — unextractable by construction.
        {"text": "The {year} {policy} supersedes the {prev_year} {policy}.",
         "gold": [{"s": "{year} {policy}", "r": "supersedes",
                   "o": "{prev_year} {policy}"}]},
        {"text": "We replaced the {prev_year} {policy} with the {year} {policy}.",
         "gold": [{"s": "{year} {policy}", "r": "supersedes",
                   "o": "{prev_year} {policy}"}]},
    ],
    "negation": [
        {"text": "{person} is no longer on the {team}.", "gold": []},
        {"text": "{company} isn't a customer any more.", "gold": []},
        {"text": "We never went ahead with the {company} partnership.", "gold": []},
    ],
    "transient": [
        {"text": "Long call with {person} today, pretty draining.", "gold": []},
        {"text": "Need to send {person} the deck before Thursday.", "gold": []},
        {"text": "{company} seem happy at the moment.", "gold": []},
        {"text": "Might restructure the {team} in the new year.", "gold": []},
    ],
    "implicit": [
        {"text": "Sent the quarterly report to {person} at {company} again.",
         "gold": [{"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "Picked {person} up from the {city} office.",
         "gold": [{"s": "{person}", "r": "located_in", "o": "{city}"}]},
        {"text": "Copied {person} in, since they run the {team}.",
         "gold": [{"s": "{person}", "r": "member_of", "o": "{team}"}]},
        {"text": "Booked the {city} room for the {company} review again.",
         "gold": [{"s": "{company}", "r": "located_in", "o": "{city}"}]},
        {"text": "Escalated to {person}, the {role}, as usual.",
         "gold": [{"s": "{person}", "r": "has_role", "o": "{role}"}]},
    ],
    "ambiguous": [
        {"text": "Their lead on the {company} work left last month.", "gold": []},
        {"text": "The bigger of the two {city} accounts churned.", "gold": []},
        {"text": "She's moving off {company} to the other team.", "gold": []},
        {"text": "One of the {team} leads is leaving, not sure which.", "gold": []},
        {"text": "The other {role} at {company} handles that now.", "gold": []},
    ],
}

SALES = {
    "third_person": [
        {"text": "{company} signed as a customer.",
         "gold": [{"s": "{company}", "r": "customer_of", "o": "us"}]},
        {"text": "{company} is on the {tier} tier.",
         "gold": [{"s": "{company}", "r": "subscription_tier", "o": "{tier}"}]},
        {"text": "{company} bought {product}.",
         "gold": [{"s": "{company}", "r": "purchased", "o": "{product}"}]},
    ],
    "first_person": [
        {"text": "I own the {company} account now.",
         "gold": [{"s": "user", "r": "owns_account", "o": "{company}"}]},
        {"text": "I'm running point on the {company} renewal.",
         "gold": [{"s": "user", "r": "owns_account", "o": "{company}"}]},
        {"text": "{company} is mine now that {person} has moved on.",
         "gold": [{"s": "user", "r": "owns_account", "o": "{company}"}]},
        {"text": "I picked up {company} this quarter.",
         "gold": [{"s": "user", "r": "owns_account", "o": "{company}"}]},
    ],
    "novel_pred": [
        {"text": "The {contract} renews on {renews_on}.",
         "gold": [{"s": "{contract}", "r": "renews_on", "o": "{renews_on}"}]},
        {"text": "{person} is the technical contact for {company}.",
         "gold": [{"s": "{person}", "r": "technical_contact_for",
                   "o": "{company}"}]},
        {"text": "{company} is up for renewal on {renews_on}.",
         "gold": [{"s": "{company}", "r": "renews_on", "o": "{renews_on}"}]},
    ],
    "multi_fact": [
        {"text": "{company} runs {product} out of {city}.",
         "gold": [{"s": "{company}", "r": "purchased", "o": "{product}"},
                  {"s": "{company}", "r": "located_in", "o": "{city}"}]},
        {"text": "{person} is {role} at {company} and owns the {contract}.",
         "gold": [{"s": "{person}", "r": "has_role", "o": "{role}"},
                  {"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "{company} is a customer, based in {city}, on the {tier} tier.",
         "gold": [{"s": "{company}", "r": "customer_of", "o": "us"},
                  {"s": "{company}", "r": "located_in", "o": "{city}"},
                  {"s": "{company}", "r": "subscription_tier", "o": "{tier}"}]},
        {"text": "{person} at {company} handles the {contract}, which renews on "
                 "{renews_on}.",
         "gold": [{"s": "{person}", "r": "works_for", "o": "{company}"},
                  {"s": "{contract}", "r": "renews_on", "o": "{renews_on}"}]},
    ],
    "negation": [
        {"text": "{company} did not renew this year.", "gold": []},
        {"text": "We lost the {company} deal.", "gold": []},
        {"text": "{company} pulled out before signing.", "gold": []},
    ],
    "transient": [
        # The precision-critical slice for this domain. Sales notes are dense
        # with non-durable optimism, and a model that commits a deal which never
        # closed is the graph-poisoning case.
        {"text": "{company} might close by Friday if legal moves fast.", "gold": []},
        {"text": "The call with {company} went really well.", "gold": []},
        {"text": "{person} sounded keen on the phone.", "gold": []},
        {"text": "Hoping to get {company} over the line this quarter.", "gold": []},
        {"text": "Chasing {person} for a signature again.", "gold": []},
    ],
    "implicit": [
        {"text": "Sent the renewal paperwork to {person} at {company} again "
                 "this quarter.",
         "gold": [{"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "Invoiced {company} for the {product} seats again.",
         "gold": [{"s": "{company}", "r": "purchased", "o": "{product}"}]},
        {"text": "{person} approved the {company} order, as usual.",
         "gold": [{"s": "{person}", "r": "works_for", "o": "{company}"}]},
        {"text": "Renewed {company} onto {tier} for another year.",
         "gold": [{"s": "{company}", "r": "subscription_tier", "o": "{tier}"}]},
    ],
    "ambiguous": [
        {"text": "Their biggest {city} account churned last month.", "gold": []},
        {"text": "The {company} renewal slipped again.", "gold": []},
        {"text": "One of the {tier} tier accounts is wobbling.", "gold": []},
        {"text": "The other contact at {company} handles renewals now.", "gold": []},
    ],
}

BANK = {"code": CODE, "business": BUSINESS, "sales": SALES}


def cell_counts():
    return {d: {c: len(v) for c, v in cats.items()} for d, cats in BANK.items()}


if __name__ == "__main__":
    import json
    counts = cell_counts()
    total = sum(n for cats in counts.values() for n in cats.values())
    print(json.dumps(counts, indent=1))
    print(f"total templates: {total}")
