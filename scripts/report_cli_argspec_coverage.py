import importlib.util
import pathlib
import re
from collections import Counter

spec = importlib.util.spec_from_file_location("gen2", "scripts/derive_cli_argspecs.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

rows = pathlib.Path("src/server/cli_dispatch_defs_data.h").read_text()
# A dispatch row is {group, verb, method, ...}. The verb may contain HYPHENS
# ("sync-code", "blast-radius", "set-server") or be the bare token NULL for a
# group that is itself a command ("aimee use", "aimee presence"). An earlier
# pattern allowed neither and so silently dropped 15 methods -- including 9 that
# were never classified at all, because a method this never yields is a method
# nothing downstream ever asks about. Undercounting the denominator is the
# failure mode to watch for here: it makes coverage look better, not worse.
methods = sorted({m.group(3) for m in re.finditer(
    r'\{\s*"([a-z0-9_.-]+)"\s*,\s*(?:"([a-z0-9_.-]*)"|NULL)\s*,\s*"([a-z0-9_.]+)"', rows)})
served = set(re.findall(r'^\{"([a-z0-9_.]+)",',
             pathlib.Path("src/server/cli_argspec_defs_data.h").read_text(), re.M))
noargs = set(re.findall(r'"([a-z0-9_.]+)"',
             pathlib.Path("src/server/cli_marshal_defs_data.h").read_text()))

NEVER = {"local state"}
tally = Counter()
detail = Counter()
for m in methods:
    if m in served:
        tally["served: argument spec"] += 1
    elif m in noargs:
        tally["served: no arguments"] += 1
    else:
        _, why = gen.spec_for(m)
        detail[why] += 1
        if why in NEVER:
            tally["excluded by design (client-local state)"] += 1
        elif why.startswith(("multi-method", "nested", "raw argv", "derived")):
            tally["blocked: needs marshaller refactor"] += 1
        else:
            tally["blocked: generator cannot read the shape"] += 1

total = len(methods)
srv = tally["served: argument spec"] + tally["served: no arguments"]
print(f"{total} methods reachable from the CLI")
for k, n in tally.most_common():
    print(f"  {n:4d}  {k}")
print(f"\nserved: {srv}/{total} = {100*srv/total:.0f}%")
serveable = total - tally["excluded by design (client-local state)"]
print(f"served of the SERVEABLE set ({serveable}): {100*srv/serveable:.0f}%")
print("\nrefusal detail:")
for k, n in detail.most_common(12):
    print(f"  {n:4d}  {k}")
