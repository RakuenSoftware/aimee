# Final request admission latency

The first final-byte admission benchmark measured several milliseconds of added
latency despite the Go admission handler taking about 2.2 microseconds. The Go
module consumer exponentially backed off from 1 to 10 ms between ring polls.
Ordinary provider requests were spaced far enough apart to reach that ceiling.
This is an identified source of potential discovery delay; complete provider
latency also includes the host, other modules and OS scheduling.

`ModuleProcessConfig.MaxIdlePollInterval` now lets a Go module cap its scheduled
idle backoff between 1 and 10 ms. Zero retains the existing 10 ms ceiling. Only
economizer selects 1 ms, because final admission precedes provider dispatch.
The existing C bus, its wire protocol, host polling and admission decisions are
unchanged. No additional spinning loop or per-request goroutine is introduced.

Go race tests pass for the client/runtime, economizer and module registry.
Virtual-clock tests inject a request after a long idle stretch, check the
configured discovery interval, and bound the number of idle polls. They also
verify that other modules retain the default. Real C-host/Go-process conformance
passes, including fragmentation, deadlines and cancellation. The shipped Go
admission stage returns the correct exact-fit/overflow decision and metadata
commitment through the C caller. This exposed a stale conformance grant listing
only five economizer stages; the harness now advertises all eight.

The independently exported economizer entry point now uses the same owner-defined
polling constant as the bundled multicall. Its export previously referenced a
nonexistent `Handle` function; it now uses the public `NewHandler` constructor.
The generated Go executable builds and passes the real C caller's admission
conformance checks. This export repair and replacing the bundled 1ms literal
with the same 1ms constant do not change the bundled admission algorithm.

The provider benchmark additionally measures process user/system CPU ticks over
10 seconds without issuing requests. The previous image's controlled rerun
passes 200 correctness checks and measures 0.03 CPU seconds over 10 wall seconds
(0.30% of one core, with a 100 Hz tick counter). The candidate image, corresponding
latency comparison and fresh deployment validation are pending. A shorter poll
interval trades more idle wakeups for less discovery delay; neither CPU cost nor
MR-18 performance acceptance is assumed from the unit tests.
