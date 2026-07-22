#!/usr/bin/env python3
"""Static plant gate for the no-shell status-authority runtime image."""

from pathlib import Path
import re
import sys


def main() -> int:
    text = Path("Dockerfile.status-authority").read_text(encoding="utf-8")
    required = {
        "dedicated build": r"make -C src status-authority-core",
        "scratch runtime": r"(?m)^FROM scratch\s*$",
        "allowlisted copy": r"COPY --from=build /runtime /",
        "non-root runtime": r"(?m)^USER 65532:65532\s*$",
        "authority entrypoint": r'ENTRYPOINT \["/usr/libexec/aimee/aimee-kb-status-authority"\]',
        "forbidden-artifact gate": r"! find /runtime",
    }
    failed = [name for name, pattern in required.items() if not re.search(pattern, text)]
    runtime = text.split("\nFROM scratch\n", 1)
    if len(runtime) != 2:
        failed.append("single scratch final stage")
    else:
        for forbidden in ("RUN ", "apt-get", "COPY . .", "/bin/sh"):
            if forbidden in runtime[1]:
                failed.append(f"runtime contains {forbidden.strip()}")
    if failed:
        print("status-authority packaging: failed: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("status-authority packaging: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
