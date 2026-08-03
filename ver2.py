import json, glob, os
for f in sorted(glob.glob("/opt/bench/results/cells/aimee__*/aimee-readiness.json")):
    d = json.load(open(f)); rt = d.get("runtime") or {}
    print("  %-16s server_version=%-28s source=%s" % (
        os.path.basename(os.path.dirname(f)).replace("aimee__","").replace("__r1",""),
        str(rt.get("server_version"))[:28], rt.get("server_version_source")))
