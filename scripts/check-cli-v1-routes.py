#!/usr/bin/env python3
"""Guard that src/cli_v1_routes_gen.inc is in sync with the server registry.

The thin client's method -> /v1 route map is generated from
src/server/server_http_routes.inc by scripts/gen-cli-v1-routes.py and committed.
This check regenerates it into a temp file and diffs, so a registry change that
isn't reflected in the committed map fails `make lint` instead of silently
leaving the client without its first-class /v1 route.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "scripts" / "gen-cli-v1-routes.py"
COMMITTED = ROOT / "src" / "cli_v1_routes_gen.inc"


def main() -> int:
    with tempfile.NamedTemporaryFile("r", suffix=".inc", delete=True) as tmp:
        subprocess.run([sys.executable, str(GEN), tmp.name], check=True,
                       stdout=subprocess.DEVNULL)
        fresh = Path(tmp.name).read_text(encoding="utf-8")
    if not COMMITTED.exists():
        print("check-cli-v1-routes: FAIL — src/cli_v1_routes_gen.inc is missing; "
              "run scripts/gen-cli-v1-routes.py")
        return 1
    if COMMITTED.read_text(encoding="utf-8") != fresh:
        print("check-cli-v1-routes: FAIL — src/cli_v1_routes_gen.inc is stale vs "
              "server_http_routes.inc; run scripts/gen-cli-v1-routes.py and commit.")
        return 1
    n = sum(1 for ln in fresh.splitlines() if ln.lstrip().startswith('{"'))
    print(f"check-cli-v1-routes: ok ({n} client /v1 routes in sync)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
