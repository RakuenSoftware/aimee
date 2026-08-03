"""Build a synthetic but internally consistent entity inventory for the
business and sales domains.

Step 2 of HARNESS_DESIGN.md. The code domain mines real facts from git; business
and sales have no equivalent source, so their entities are invented here.

WHY INVENTING ENTITIES IS ACCEPTABLE AND INVENTING LABELS IS NOT. The danger of
hand-authored gold is that the labeller resolves an AMBIGUOUS case one way and
enshrines that reading as truth — which is exactly what happened to `mf04` in the
original 70, where "the build host is called forge" could reasonably be labelled
with either `build host` or `forge` as subject. A generated
`Acme Freight | customer_of | us` has no such ambiguity: the generator decided the
fact BEFORE writing the sentence, so there is nothing to misread. What synthetic
entities cost is realism, not correctness, and that limit is recorded rather than
hidden.

CONSISTENCY IS THE POINT. Entities are generated once into a closed world and
then referenced, so `Marta Kovac` works for the same company in every note she
appears in, and `Halden Logistics` has one location everywhere. Without that, a
multi-fact note and a later single-fact note about the same person could assert
contradictory things, and a model that correctly extracted both would be marked
wrong twice.

Names are drawn from a wide pool of given/family name parts and combined
seeded-randomly. Collisions with real people are possible and meaningless: these
are labels in a closed fictional world, and no note attaches a real person's
attributes to them.
"""
import argparse
import json
import random

GIVEN = """Marta Priya Ingrid Klaus Sofia Tomas Aisha Niels Renata Jonas Lucia Otto
Hana Emil Nadia Viktor Elena Marcus Yuki Farid Greta Anton Leila Bram Saskia Rune
Camille Dario Freya Idris Katya Lars Mira Nolan Orla Pavel Quinn Rosa Stefan Tara
Ulrich Vera Wren Xenia Yusuf Zara Bea Cato Dilan Esme""".split()

FAMILY = """Kovac Lindqvist Okonkwo Bauer Marchetti Halvorsen Nakamura Duarte
Vasquez Fenwick Aaltonen Brennan Castellanos Dvorak Eriksen Fontaine Gallagher
Haugen Ibrahim Jankowski Kristensen Lombardi Moreau Nyberg Ostrowski Petrov
Quintero Rasmussen Sorensen Tanaka Ueda Virtanen Wexler Ximenes Yilmaz Zielinski
Ashford Blackwood Carrington Delacroix""".split()

COMPANY_HEAD = """Halden Northwind Corvo Aldridge Brightwater Cormorant Dunmore
Everline Fairweather Grimsby Hollowell Ironbridge Juniper Kestrel Larkspur
Marrowfield Nightingale Oakhaven Pinehurst Quarrymill Redgrave Stonebridge
Thornbury Uplands Vanbrugh Westmere Yarrow Ashcombe Belmont Cranfield""".split()

COMPANY_TAIL = """Logistics Freight Analytics Systems Robotics Diagnostics
Materials Interactive Networks Foods Marine Aviation Textiles Instruments
Chemicals Publishing Surveying Bioscience Optics Energy""".split()

CITY = """Wellington Auckland Christchurch Dunedin Oslo Bergen Helsinki Tampere
Utrecht Ghent Porto Valencia Bologna Graz Aarhus Malmo Gdansk Brno Tallinn Vilnius
Cork Dundee Leeds Bristol Nantes Lyon Bilbao Trieste Bratislava Ljubljana""".split()

PRODUCT_HEAD = """Atlas Beacon Cinder Delta Ember Fathom Girder Harbour Ingot
Junction Keel Lantern Mercator Nimbus Orrery Pylon Quarry Ridge Sextant Tessera""".split()

PRODUCT_TAIL = "Platform Suite Gateway Console Engine Ledger Router Archive".split()

ROLE = ["head of platform", "principal engineer", "account manager",
        "solutions architect", "commercial lead", "operations manager",
        "technical contact", "finance lead", "delivery manager",
        "procurement lead", "support engineer", "regional director"]

TEAM = ["retrieval team", "platform team", "revenue committee",
        "pricing working group", "security working group", "steering group",
        "architecture board", "renewals desk", "field engineering",
        "customer success"]

TIER = ["starter", "standard", "professional", "enterprise", "government"]
MONTH = ["January", "February", "March", "April", "May", "June", "July",
         "August", "September", "October", "November", "December"]


def build(seed, n_people, n_companies, n_products):
    rng = random.Random(seed)

    companies, used = [], set()
    while len(companies) < n_companies:
        name = f"{rng.choice(COMPANY_HEAD)} {rng.choice(COMPANY_TAIL)}"
        if name in used:
            continue
        used.add(name)
        companies.append({
            "name": name,
            "city": rng.choice(CITY),
            # A company is either a customer of ours or a vendor to us, never
            # both: a note asserting each would be a contradiction the gold
            # could not represent.
            "relationship": rng.choice(["customer", "customer", "customer", "vendor"]),
            "tier": rng.choice(TIER),
        })

    people, pused = [], set()
    while len(people) < n_people:
        name = f"{rng.choice(GIVEN)} {rng.choice(FAMILY)}"
        if name in pused:
            continue
        pused.add(name)
        # Every person has exactly ONE employer and ONE role, fixed for the whole
        # corpus, so repeated references cannot contradict each other.
        emp = rng.choice(companies)
        people.append({
            "name": name,
            "employer": emp["name"],
            "employer_city": emp["city"],
            "role": rng.choice(ROLE),
            "team": rng.choice(TEAM),
        })

    products = []
    for _ in range(n_products):
        products.append({
            "name": f"{rng.choice(PRODUCT_HEAD)} {rng.choice(PRODUCT_TAIL)}",
            "tier": rng.choice(TIER),
        })

    contracts = []
    for c in companies:
        if c["relationship"] != "customer":
            continue
        contracts.append({
            "name": f"{c['name'].split()[0]} contract",
            "company": c["name"],
            "renews_on": f"{rng.randint(1, 28)} {rng.choice(MONTH)}",
            "tier": c["tier"],
            "owner": rng.choice(people)["name"],
        })

    return {"people": people, "companies": companies,
            "products": products, "contracts": contracts,
            "seed": seed}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260801)
    ap.add_argument("--people", type=int, default=800)
    ap.add_argument("--companies", type=int, default=300)
    ap.add_argument("--products", type=int, default=120)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    inv = build(args.seed, args.people, args.companies, args.products)
    with open(args.out, "w") as fh:
        json.dump(inv, fh, indent=1, ensure_ascii=False)

    cust = sum(1 for c in inv["companies"] if c["relationship"] == "customer")
    print(f"people      {len(inv['people'])}")
    print(f"companies   {len(inv['companies'])}  ({cust} customers)")
    print(f"products    {len(inv['products'])}")
    print(f"contracts   {len(inv['contracts'])}")
    print(f"seed        {inv['seed']}")


if __name__ == "__main__":
    main()
