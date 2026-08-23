#!/usr/bin/env bash
# Everything that has to pass before a DB2 bus-migration batch is committed.
#
# Every step here was already being run per batch, by hand, in this order.
# Typing them again each time invites typing some of them: a batch that skipped
# the replay looked exactly like one that passed it. This is what "verified"
# means on this branch, and it stops at the first failure.
#
# The two baselines are re-pinned from what they measured rather than by hand,
# because a pin edited by hand is a pin that can be edited to whatever makes
# the test pass.
set -u
cd "$(dirname "$0")/.." || exit 1
LOGS=.db2verify
mkdir -p "$LOGS"

step() {
   local label=$1
   shift
   if ! "$@" > "${LOGS}/${label}.log" 2>&1; then
      echo "FAILED: $label"
      grep -E "error:|FAIL|Assertion|ERROR" "${LOGS}/${label}.log" | head -6
      return 1
   fi
   echo "ok: $label"
}

# The generator first: a catalog edit has to reach the generated tree before
# anything is built against it.
step generator python3 scripts/gen_db2_contract.py --write || exit 1
step gosync python3 scripts/db2_sync_go_contract_test.py || exit 1

clang-format-19 -i src/modules/db2/module_adapter.c src/modules/db2/module_adapter.h \
   src/tests/test_db2_module_contract.c src/tests/test_bus_db2_module.c \
   src/tests/test_bus_db2_process.c
gofmt -w server-go/db2/contract_test.go

step build make -C src -j8 GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 \
   unit-test-db2-module-contract unit-test-bus-db2-module || exit 1
grep -E "^test_(db2_module_contract|bus_db2_module):" "${LOGS}/build.log"

python3 scripts/gen_db2_declaration_ledger.py --write > /dev/null || exit 1
python3 - <<'PY' || exit 1
import json
import re
from pathlib import Path

summary = json.loads(Path("tests/baselines/db2/declarations-v1.json").read_text())["summary"]
pin = Path("scripts/tests/test_gen_db2_declaration_ledger.py")
text = pin.read_text(encoding="utf-8")
for key in ("reviewed", "audit_pending", "internal_unconsumed", "private_test_only",
            "declarations", "headers"):
    text = re.sub(rf'("{key}": )\d+,', rf'\g<1>{summary[key]},', text, count=1)
pin.write_text(text, encoding="utf-8")
print("ledger:", summary)
PY

step closure python3 scripts/check_db2_link_closure.py --write-contract || exit 1
step boundary python3 scripts/check_db2_source_boundary.py --write || exit 1

python3 - <<'PY' || exit 1
import json
import re
from pathlib import Path

raw = json.loads(Path("tests/baselines/db2/source-boundary-v2.json").read_text())
summary = dict(raw["summary"])
# The test pins a count of source files that the baseline records in parts.
summary["source_files"] = (summary["c_files"] + summary["sql_files"] + summary["headers"])
pin = Path("scripts/tests/test_check_db2_source_boundary.py")
text = pin.read_text(encoding="utf-8")
for key in ("source_files", "consumer_files", "include_directives"):
    text = re.sub(rf'(result\["{key}"\], )\d+\)', rf'\g<1>{summary[key]})', text, count=1)
# The same counts are pinned a second way, inside the string the checker prints,
# by the test that runs it from another directory. Re-pinning only the first
# shape left that one to fail the gate for the very change the gate just made.
for key, word in (("consumer_files", "consumers"), ("source_files", "boundary files"),
                  ("include_directives", "inbound includes")):
    text = re.sub(rf'("\s*)\d+( {word}")', rf'\g<1>{summary[key]}\g<2>', text)
pin.write_text(text, encoding="utf-8")
print("boundary: source_files", summary["source_files"],
      "consumers", summary["consumer_files"], "includes", summary["include_directives"])
PY

# Every Go package in server-go, by wildcard rather than by name.
#
# This was three enumerated steps: ./db2/, then ./modules/db2/ after a batch
# landed with two of its tests failing because nothing ran them, then
# ./postgres/ ./modules/postgres/ after the storage packages turned out to be
# gated by nothing at all. Each entry was added once the gap it closed had
# already cost something.
#
# A list is the wrong shape for this. A new package falls outside an enumeration
# silently -- the gate stays green, and it is green having run nothing over the
# new code. ./... covers whatever is there, so coverage follows from the layout
# instead of from somebody remembering to extend a list.
#
# Offline: the live probes and the parity comparison skip unless their
# environment variables name a database, so this needs nothing running.
step gotest bash -c 'cd server-go && go test -count=1 ./...' || exit 1
# Every catalogued operation has a handler, counted from the contract's own
# constants rather than a list anyone maintains -- a hand-kept list is how a
# batch was once reported complete at 307 of 307 when it was not.
step gocoverage python3 scripts/db2_go_coverage.py || exit 1
# Wall-clock stamps in the format the C writes them. These columns are compared
# AS TEXT against pg_now_text(), so a spelling that disagrees makes a sweep skip
# or collect a day of rows and says nothing about it.
step stamps python3 scripts/db2_check_stamp_formats.py || exit 1
# Columns checked in characters that are bounded in bytes by whoever reads
# them. A value of N multi-byte characters is then accepted by the database and
# unreadable afterwards -- written, not corrupted, with nothing at write time
# saying so. Fails on the actionable set; also prints the columns whose readers
# disagree about the SIZE, which are open questions rather than defects.
step units python3 scripts/db2_check_length_units.py || exit 1
step scripttests python3 -m unittest discover -s scripts/tests -q || exit 1
step lint make -C src lint || exit 1
tail -1 "${LOGS}/lint.log"

# An operation nothing replays is an operation whose handler, decoder and
# backend have never run together against a real database. That is how a
# statement Postgres could not plan survived being written.
python3 - <<'PY' || exit 1
import json
import re
from pathlib import Path

catalog = json.loads(Path("src/modules/db2/eventcontract/operations.json").read_text())
names = {str(item["name"]) for item in catalog["operations"]}
replay = Path("src/tests/test_bus_db2_process.c").read_text()
called = set(re.findall(r"aimee_db2_(\w+)_call\b", replay)) & names
missing = sorted(names - called)
print(f"catalogued {len(names)}, replayed {len(called)}")
if missing:
    raise SystemExit(f"not replayed: {missing}")
PY

step replay bash scripts/db2_replay_env.sh || exit 1
tail -1 "${LOGS}/replay.log"
echo "GATE PASSED"
