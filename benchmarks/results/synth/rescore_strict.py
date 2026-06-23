#!/usr/bin/env python3
"""rescore_strict.py RES4.json — re-score saved raw extractions with a STRICT, intuitive
rubric: a malformed primary artifact (doc_summary / code_unit) costs points, and a
validity-rate term penalizes any malformed artifact directly. No re-generation needed."""
import json, sys, re
ST={"draft","accepted","done","rejected","deferred"}; PR={"low","medium","high"}
CK={"fact","requirement","constraint","decision","behavior"}
EK={"component","system","concept","protocol","data_store","tool"}
def repair(s):
    s=s.strip()
    if "</think>" in s: s=s.rsplit("</think>",1)[1].strip()
    if s.startswith("```"): s="\n".join(s.splitlines()[1:]); s=s.rsplit("```",1)[0]
    a=s.find("{")
    if a<0: return s
    out=[]; stack=[]; ins=False; esc=False
    for c in s[a:]:
        if ins:
            out.append(c)
            if esc: esc=False
            elif c=="\\": esc=True
            elif c=='"': ins=False
        else:
            if c=='"': ins=True; out.append(c)
            elif c in "{[": stack.append(c); out.append(c)
            elif c in "}]":
                want="{" if c=="}" else "["
                if stack and stack[-1]==want:
                    stack.pop(); out.append(c)
                    if not stack: break
            else: out.append(c)
    while stack: out.append("}" if stack.pop()=="{" else "]")
    res="".join(out); return re.sub(r',(\s*[}\]])', r'\1', res)

def art_valid(x):
    if not isinstance(x,dict): return False
    k=x.get("kind"); pl=x.get("payload")
    if not isinstance(pl,dict): return False
    if k=="doc_summary": return pl.get("status") in ST and pl.get("priority") in PR and isinstance(pl.get("components"),list) and bool(pl.get("summary"))
    if k=="claim": return pl.get("claim_kind") in CK and all(pl.get(z) for z in ("subject","attribute","value","text"))
    if k=="entity": return pl.get("entity_kind") in EK and bool(pl.get("name"))
    return False

def score_doc_strict(o):
    a=o.get("artifacts") if isinstance(o,dict) else None
    if not isinstance(a,list) or not a: return 0.0
    ds = 1.0 if any(isinstance(x,dict) and x.get("kind")=="doc_summary" and art_valid(x) for x in a) else 0.0
    cl = 1.0 if any(isinstance(x,dict) and x.get("kind")=="claim" and art_valid(x) for x in a) else 0.0
    nv = sum(1 for x in a if art_valid(x))
    prec = nv/len(a)                       # validity rate: malformed artifacts cost directly
    return (ds+cl+prec)/3.0

def score_code_strict(o):
    a=o.get("artifacts") if isinstance(o,dict) else None
    if not isinstance(a,list) or not a: return 0.0
    cu=[x for x in a if isinstance(x,dict) and x.get("kind")=="code_unit" and isinstance(x.get("payload"),dict)]
    if not cu: return 0.0
    pl=cu[0]["payload"]
    fields=[bool(pl.get("summary")), isinstance(pl.get("side_effects"),list), isinstance(pl.get("domain_concepts"),list)]
    base=sum(fields)/3.0                    # fraction of required fields well-formed (no 0.4 floor)
    # penalty if it emitted junk beyond the one code_unit
    extra=len(a)-1
    return base*(1.0 if extra<=0 else max(0.6, 1.0-0.1*extra))

r=json.load(open(sys.argv[1]))
docs=[d for d in r["detail"] if d.get("role")=="extract_doc"]
code=[d for d in r["detail"] if d.get("role")=="extract_code"]
def parse(d):
    try: return json.loads(repair(d.get("raw","")))
    except Exception: return None
dd=[score_doc_strict(parse(d)) for d in docs if d.get("raw") is not None]
cc=[(d.get("lang"),score_code_strict(parse(d))) for d in code if d.get("raw") is not None]
import statistics
ndoc=round(statistics.mean(dd),3) if dd else 0
ncode=round(statistics.mean([s for _,s in cc]),3) if cc else 0
by={}
for lang,s in cc: by.setdefault(lang,[]).append(s)
by={k:round(statistics.mean(v),3) for k,v in sorted(by.items())}
print(f"{r['name']}:  OLD doc={r['doc_schema']} code={r['code_schema']}   ->   STRICT doc={ndoc} code={ncode}")
print(f"  strict code_by_lang: {by}")
