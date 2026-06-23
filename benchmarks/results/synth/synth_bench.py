#!/usr/bin/env python3
"""synth_bench.py PORT MODEL NAME [CORPUS] — GRAMMAR-ENFORCED curator/synth benchmark.

Passes a generic JSON GBNF via the `grammar` field so output is *syntactically valid
JSON by construction* (NOT response_format=json_object, which b9761 silently ignores).
raw_parse should be 1.0; doc_schema/code_schema then measure content (valid enums /
required fields) free of any validity confound. Saves every raw extraction for the
strict re-score (rescore_strict.py) and the content judge.

CORPUS (argv[4]) is the workspace sample set; it is NOT committed (built per-host from
~/dev + ~/gow). See README.md. Run via serve_and_bench.sh, which starts llama-server
and invokes this. Output: res4_<NAME>.json + a one-line summary."""
import json, sys, time, urllib.request, textwrap, re
PORT,MODEL,NAME=sys.argv[1],sys.argv[2],sys.argv[3]
CORPUS=sys.argv[4] if len(sys.argv)>4 else "synth_corpus_full.json"
samples=json.load(open(CORPUS))

# Standard llama.cpp generic-JSON grammar; root is an object => always a parseable JSON object.
JSON_GBNF = r'''
root   ::= object
value  ::= object | array | string | number | ("true" | "false" | "null") ws
object ::= "{" ws ( string ":" ws value ("," ws string ":" ws value)* )? "}" ws
array  ::= "[" ws ( value ("," ws value)* )? "]" ws
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt/] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) )* "\"" ws
number ::= ("-"? ([0-9] | [1-9] [0-9]{0,15})) ("." [0-9]+)? ([eE] [-+]? [0-9]{1,16})? ws
ws ::= | " " | "\n" [ \t]{0,20}
'''

def doc_prompt(i):
    return textwrap.dedent(f"""
    You are a knowledge extraction assistant. Extract structured knowledge from the
    document chunk below and return ONLY a valid JSON object — no markdown fences.
    Document: {i.get('file_path','')}
    Section:  {i.get('heading_path','') or '(top level)'}
    Content:
    {i.get('content','')}
    Return a JSON object {{"artifacts":[...]}} where each artifact has "kind" in
    [doc_summary, claim, entity], a "payload", a "confidence" (>=0.70), "citations".
    doc_summary.payload: summary, source_file, status(draft|accepted|done|rejected|deferred),
    priority(low|medium|high), components(list). claim.payload: subject, attribute, value,
    claim_kind(fact|requirement|constraint|decision|behavior), text. entity.payload: name,
    entity_kind(component|system|concept|protocol|data_store|tool), context. Always include
    >=1 doc_summary and >=1 claim; one entity per named system/component.""").strip()
def code_prompt(i):
    return textwrap.dedent(f"""
    You are a code analysis assistant. Analyze the {i.get('kind','function')} below and
    return ONLY a valid JSON object — no markdown fences.
    File: {i.get('file_path','')}
    Symbol: {i.get('symbol','')} at line {i.get('line',0)}
    Body:
    {i.get('body','')}
    Return {{"artifacts":[{{"kind":"code_unit","payload":{{"summary":"1-2 sentence intent",
    "invariants":"assumptions/guarantees or empty","side_effects":["filesystem","allocation"],
    "domain_concepts":["concept"]}},"confidence":0.82,"citations":[]}}]}}""").strip()
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
    res="".join(out)
    res=re.sub(r',(\s*[}\]])', r'\1', res)
    return res
DK={"doc_summary","claim","entity"}; ST={"draft","accepted","done","rejected","deferred"}
PR={"low","medium","high"}; EK={"component","system","concept","protocol","data_store","tool"}
CK={"fact","requirement","constraint","decision","behavior"}
def score_doc(o):
    a=o.get("artifacts") if isinstance(o,dict) else None
    if not isinstance(a,list) or not a: return 0.0,0
    p=[1.0 if any(isinstance(x,dict) and x.get("kind")=="doc_summary" for x in a) else 0.0,
       1.0 if any(isinstance(x,dict) and x.get("kind")=="claim" for x in a) else 0.0]
    nv=0
    for x in a:
        if not isinstance(x,dict): continue
        k=x.get("kind"); pl=x.get("payload",{})
        if k not in DK or not isinstance(pl,dict): continue
        ok=True
        if k=="doc_summary": ok=pl.get("status") in ST and pl.get("priority") in PR and isinstance(pl.get("components"),list) and bool(pl.get("summary"))
        elif k=="claim": ok=pl.get("claim_kind") in CK and all(pl.get(z) for z in ("subject","attribute","value","text"))
        elif k=="entity": ok=pl.get("entity_kind") in EK and bool(pl.get("name"))
        if ok: nv+=1
    p.append(min(1.0,nv/3.0)); return sum(p)/len(p),nv
def score_code(o):
    a=o.get("artifacts") if isinstance(o,dict) else None
    if not isinstance(a,list) or not a: return 0.0,0
    cu=[x for x in a if isinstance(x,dict) and x.get("kind")=="code_unit"]
    if not cu: return 0.0,0
    pl=cu[0].get("payload",{})
    ok=isinstance(pl,dict) and bool(pl.get("summary")) and isinstance(pl.get("side_effects"),list) and isinstance(pl.get("domain_concepts"),list)
    return (1.0 if ok else 0.4),(1 if ok else 0)
def call(prompt):
    if "qwen" in NAME.lower(): prompt=prompt+" /no_think"
    body=json.dumps({"model":MODEL,"messages":[{"role":"user","content":prompt}],"temperature":0,
      "max_tokens":1536,"chat_template_kwargs":{"enable_thinking":False},"grammar":JSON_GBNF}).encode()
    req=urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/chat/completions",data=body,headers={"Content-Type":"application/json"})
    t0=time.time(); d=json.load(urllib.request.urlopen(req,timeout=180)); dt=time.time()-t0
    return d["choices"][0]["message"]["content"] or "", dt, d.get("usage",{}).get("completion_tokens",0)
res=[]; TT=0; TOK=0; raw_parse=0; cap=[]
for s in samples:
    pr=doc_prompt(s["input"]) if s["role"]=="extract_doc" else code_prompt(s["input"])
    lang=s.get("lang")
    try: txt,dt,tk=call(pr)
    except Exception as e: res.append({"role":s["role"],"lang":lang,"parsed":False,"score":0,"valid":0,"err":str(e)[:80]}); continue
    TT+=dt; TOK+=tk
    try: json.loads(txt.strip()); raw_parse+=1
    except: pass
    try: o=json.loads(repair(txt)); pa=True
    except: o={}; pa=False
    sc,nv=(score_doc(o) if s["role"]=="extract_doc" else score_code(o))
    res.append({"role":s["role"],"lang":lang,"project":s.get("project"),
                "file":s["input"].get("file_path"),"parsed":pa,"score":round(sc,2),"valid":nv,"toks":tk,
                "raw":txt})   # full extraction saved for manual review + future judge round
    if len(cap)<2: cap.append({"role":s["role"],"raw":txt[:600]})
d=[r for r in res if r["role"]=="extract_doc"]; c=[r for r in res if r["role"]=="extract_code"]
av=lambda x,k: round(sum(r.get(k,0) for r in x)/len(x),3) if x else 0
# per-language code breakdown
code_by_lang={}
for r in c:
    code_by_lang.setdefault(r.get("lang") or "?",[]).append(r["score"])
code_by_lang={k:round(sum(v)/len(v),3) for k,v in sorted(code_by_lang.items())}
out={"name":NAME,"doc_schema":av(d,"score"),"code_schema":av(c,"score"),"parse_after_repair":av(res,"parsed"),
     "raw_parse_rate":round(raw_parse/len(res),3),"tok_per_s":round(TOK/TT,1) if TT else 0,"n":len(res),
     "n_doc":len(d),"n_code":len(c),"code_by_lang":code_by_lang,"detail":res,"samples":cap}
json.dump(out,open(f"/mnt/media/synthbench/res4_{NAME}.json","w"),indent=1)
print(f"{NAME}: doc={out['doc_schema']}(n{out['n_doc']}) code={out['code_schema']}(n{out['n_code']}) raw_parse={out['raw_parse_rate']} {out['tok_per_s']}tok/s | by_lang={out['code_by_lang']}")
