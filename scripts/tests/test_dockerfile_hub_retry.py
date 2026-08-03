"""Prove the Dockerfile's hub-fetch retry retries transient errors and only those.

The function under test lives inside a Dockerfile heredoc, so it is extracted and
exec'd here rather than duplicated -- a copy would drift from what the build runs,
which is the whole failure mode this is guarding against.
"""
import pathlib
import re
import sys
import types

src = pathlib.Path("Dockerfile").read_text(encoding="utf-8")
fn_start = src.index("def fetch(repo, **kw):")
fn_end = src.index('raise AssertionError("unreachable")', fn_start) + len('raise AssertionError("unreachable")')
# Only the function. Its module-level neighbours read embedders.json, which is not
# what is under test here.
code = "import time\n" + src[fn_start:fn_end]

mod = types.ModuleType("baked")
calls = {"n": 0, "sleeps": []}


def fake_sleep(s):
    calls["sleeps"].append(s)


class Boom(Exception):
    pass


exec(compile(code, "Dockerfile:PYBAKE", "exec"), mod.__dict__)
mod.time = types.SimpleNamespace(sleep=fake_sleep)

# 1. a transient failure is retried and then succeeds
seq = ["429 Too Many Requests for url ...", None]


def flaky(repo, **kw):
    calls["n"] += 1
    err = seq[calls["n"] - 1] if calls["n"] <= len(seq) else None
    if err:
        raise Boom(err)
    return "/cache/" + repo


mod.snapshot_download = flaky
assert mod.fetch("hotchpotch/bekko-embedding-v1-a25m") == "/cache/hotchpotch/bekko-embedding-v1-a25m"
assert calls["n"] == 2, calls
assert calls["sleeps"] == [15], calls
print("  transient 429 is retried and succeeds")

# 2. a permanent failure is NOT retried -- waiting to repeat a typo helps nobody
calls.update(n=0, sleeps=[])


def permanent(repo, **kw):
    calls["n"] += 1
    raise Boom("RepositoryNotFoundError: 401 Client Error. Repository not found")


mod.snapshot_download = permanent
try:
    mod.fetch("nobody/does-not-exist")
except Boom:
    pass
else:
    sys.exit("a permanent error must propagate")
assert calls["n"] == 1, calls
assert calls["sleeps"] == [], calls
print("  permanent error fails on the first attempt")

# 3. a persistently transient failure gives up with backoff, not forever
calls.update(n=0, sleeps=[])


def always_429(repo, **kw):
    calls["n"] += 1
    raise Boom("503 Server Error")


mod.snapshot_download = always_429
try:
    mod.fetch("hotchpotch/bekko-embedding-v1-a25m")
except Boom:
    pass
else:
    sys.exit("an exhausted retry must propagate")
assert calls["n"] == 5, calls
assert calls["sleeps"] == [15, 30, 60, 120], calls
print("  exhausted retries give up after 5 attempts with exponential backoff")
print("retrytest: ok")
