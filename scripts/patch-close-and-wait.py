#!/usr/bin/env python3
"""Give ConcurrentModuleCaller a way to be stopped before its client detaches.

Poll reads the bus's shared-memory region from its own goroutine and Detach
unmaps it, so detaching while that goroutine is live is a read through an
unmapped page. The loop had no shutdown signal at all: it exited only when the
context ended or a read failed, so a caller wanting to detach could not wait for
it.

One-shot edit, kept as a file because the patch is too long to pass through a
shell safely.
"""

import sys
from pathlib import Path

TARGET = Path(__file__).resolve().parent.parent / "server-go" / "bus" / "concurrent_module_caller.go"

EDITS = [
    (
        """	isClosed     bool
	closed       chan struct{}
	closeOnce    sync.Once
	pollInterval time.Duration
}""",
        """	isClosed bool
	closed   chan struct{}
	// stopped is closed by the poll goroutine as it returns, so a caller can
	// wait for it to be gone before unmapping what it reads.
	stopped      chan struct{}
	closeOnce    sync.Once
	pollInterval time.Duration
}""",
    ),
    (
        """		assemblies: make(map[uint64]concurrentAssembly), closed: make(chan struct{}),
		pollInterval: 200 * time.Microsecond}""",
        """		assemblies: make(map[uint64]concurrentAssembly), closed: make(chan struct{}),
		stopped: make(chan struct{}), pollInterval: 200 * time.Microsecond}""",
    ),
    (
        """func (c *ConcurrentModuleCaller) poll(ctx context.Context) {
	idleDelay := c.pollInterval
	for {
		if err := ctx.Err(); err != nil {""",
        """func (c *ConcurrentModuleCaller) poll(ctx context.Context) {
	defer close(c.stopped)
	idleDelay := c.pollInterval
	for {
		// Stop when asked, not only when the context ends or a read fails.
		// Without this the loop cannot be shut down at all, so a caller that
		// wants to detach has no way to wait for it to stop first.
		select {
		case <-c.closed:
			return
		default:
		}
		if err := ctx.Err(); err != nil {""",
    ),
    (
        """func (c *ConcurrentModuleCaller) finish(err error) {""",
        """// CloseAndWait stops the poll loop and waits for it to have returned.
//
// MUST be called before the underlying Client is detached. Poll reads the bus's
// shared-memory region from its own goroutine and Detach unmaps it, so a detach
// while that goroutine is live is a read through an unmapped page: a segfault,
// after the work has finished and every check has passed.
//
// Nothing in the type system enforces the order, and NO IN-PROCESS TEST CAN
// FAIL ON IT -- a fake bus has no region to unmap. It was found on real
// hardware by a peer module that had been missing the call for as long as it
// had existed, across twenty-odd green runs, because the fault arrives after
// the last assertion.
func (c *ConcurrentModuleCaller) CloseAndWait() {
	c.finish(ErrModuleCallCancelled)
	<-c.stopped
}

func (c *ConcurrentModuleCaller) finish(err error) {""",
    ),
]


def main() -> int:
    body = TARGET.read_text()
    for old, new in EDITS:
        if new.split("\n")[0] in body and old not in body:
            print("patch-close-and-wait: already applied")
            return 0
        if old not in body:
            print(f"patch-close-and-wait: anchor not found:\n{old[:70]}")
            return 1
        body = body.replace(old, new, 1)
    TARGET.write_text(body)
    print("patch-close-and-wait: applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
