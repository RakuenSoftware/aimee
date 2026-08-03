import json, glob, os
for f in sorted(glob.glob("/opt/bench/results/cells/aimee__*/aimee-readiness.json")):
    cell = os.path.basename(os.path.dirname(f))
    d = json.load(open(f))
    rt = d.get("runtime") or {}
    print("  %-34s client=%s server=%s mode=%s" % (
        cell.replace("aimee__","").replace("__r1",""),
        str(rt.get("client_version") or rt.get("aimee_version") or "?")[:34],
        str(rt.get("server_version") or "?")[:34],
        d.get("readiness_mode")))
