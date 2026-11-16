#!/usr/bin/env python3
"""Print the human-readable output recorded in a `go test -json` event stream.

`go test -json` keeps the exact text `go test -v` would have printed in each
event's Output field. Printing those fields in order reproduces the -v log, so
a job can keep the machine-readable record for later verification without
losing a readable log. A line that is not a JSON event (a build failure, for
example) is printed as-is rather than dropped.
"""
import json
import sys


def main(argv):
    if len(argv) != 2:
        print("usage: go-test-json-output.py EVENTS.json", file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as events:
        for line in events:
            try:
                event = json.loads(line)
            except ValueError:
                sys.stdout.write(line)
                continue
            if isinstance(event, dict) and event.get("Action") == "output":
                sys.stdout.write(event.get("Output", ""))
            elif not isinstance(event, dict):
                sys.stdout.write(line)
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
