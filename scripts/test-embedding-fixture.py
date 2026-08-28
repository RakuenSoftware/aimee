#!/usr/bin/env python3
"""Emit a deterministic 768-dimensional vector for hermetic live-PG tests."""

import json
import sys


def main() -> int:
    # Drain stdin so the parent exercises its normal sidecar pipe lifecycle.
    sys.stdin.buffer.read()
    json.dump([1.0] + [0.0] * 767, sys.stdout, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
