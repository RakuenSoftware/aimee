#!/usr/bin/env python3
"""Stop the poll goroutine before detaching the client that it reads.

Three constructors in cmd/aimee-module follow the same shape:

    caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)  // starts a goroutine
    ...
    x, err := somethingUsing(caller)
    if err != nil {
        busClient.Detach()      // unmaps the region the goroutine is reading
        return nil, err
    }

That second Detach is the defect. The first one -- immediately after
NewConcurrentModuleCaller fails -- is safe, because no goroutine was started.

A peer module hit this on real hardware: every check green, then a fault in the
poll loop reading through an unmapped page after the last assertion. It cannot
fail in process, because a fake bus has no region to unmap.
"""

import sys
from pathlib import Path

TARGET = Path(__file__).resolve().parent.parent / "server-go" / "cmd" / "aimee-module" / "main.go"

# (constructor call, the Detach that follows it)
EDITS = [
    (
        """	store, err := db1.NewClient(caller, 0)
	if err != nil {
		busClient.Detach()
		return nil
	}""",
        """	store, err := db1.NewClient(caller, 0)
	if err != nil {
		// The caller's poll goroutine reads the region Detach unmaps, so it has
		// to be stopped and waited for first.
		caller.CloseAndWait()
		busClient.Detach()
		return nil
	}""",
    ),
    (
        """	db, err := store.NewStore(caller)
	if err != nil {
		busClient.Detach()
		return nil, err
	}""",
        """	db, err := store.NewStore(caller)
	if err != nil {
		caller.CloseAndWait()
		busClient.Detach()
		return nil, err
	}""",
    ),
    (
        """	client, err := delegatecontract.NewBusClient(caller, 0)
	if err != nil {
		busClient.Detach()
		return nil, err
	}""",
        """	client, err := delegatecontract.NewBusClient(caller, 0)
	if err != nil {
		caller.CloseAndWait()
		busClient.Detach()
		return nil, err
	}""",
    ),
]


def main() -> int:
    body = TARGET.read_text()
    applied = 0
    for old, new in EDITS:
        if old not in body:
            if "caller.CloseAndWait()" in body:
                continue
            print(f"patch-detach-order: anchor not found:\n{old[:60]}")
            return 1
        body = body.replace(old, new, 1)
        applied += 1
    TARGET.write_text(body)
    print(f"patch-detach-order: {applied} detach site(s) now stop the poll loop first")
    return 0


if __name__ == "__main__":
    sys.exit(main())
