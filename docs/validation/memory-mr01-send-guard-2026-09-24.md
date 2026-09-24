# MR-01 HTTP send guard transport prerequisite — 2026-09-24

The POSIX HTTP client now has explicit per-call acquire/release callbacks for
buffered and streaming byte requests. Acquisition occurs after connection and
request construction, immediately before the first request write. Refusal
returns the existing non-retryable admission status, with zero request bytes
written. Release runs on refusal or write completion, before response reads.
Ordinary calls pass no guard, including nested owner HTTP calls. An optional
monotonic acquisition/write timeout bounds the guard separately from the
provider response timeout; acquisition that uses up that budget refuses before
writing.

The native `unit-test-http-send-guard` target drives real loopback sockets for
both buffered and streaming paths. It verifies refusal sends zero bytes,
successful requests reach the peer, release completes before the peer replies,
ordinary HTTP calls made during acquisition do not inherit the guard, and
acquisition timeout sends zero bytes in both transport modes.
The target is included in the native test list. It passes with the actual
POSIX transport compiled under the repository warning/error flags.

This is a transport prerequisite only. Provider call sites do not yet install
a storage guard. It does not close the post-check mutation race or MR-01 A5.
