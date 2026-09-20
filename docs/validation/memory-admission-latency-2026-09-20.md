# Final request admission latency

The first final-byte admission benchmark measured several milliseconds of added
latency despite the Go admission handler taking about 2.2 microseconds in the
local microbenchmark. The Go
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

Application and matrix harness `f110e9b873` pass **1,093/1,093** checks in fresh
`.253` CT 9498 deployments: **683 T2** and **410 T3**. Each placement retains all
200 provider-boundary checks, including exact byte fits, refusal without dispatch,
streaming failures and complete projection/constraint/tool preservation. Existing
semantic, concurrency, isolation, review, restart, rollback and outage gates pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-f110e9b873.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-f110e9b873.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-f110e9b873.json)
and [all nine image identities](memory-shared-reliability-2026-09-20/image-identities-f110e9b873.json)
are committed. Application image:
`sha256:d052b83facdc771df0afff8422d05554faf864c3b3836202b87ab082dde0e619`.
The PostgreSQL and embedder images match the previous validated run. The later
export-entry-point repair is validated by a complete independent export, Go build
and real C caller; the bundled process retains exactly the tested 1ms setting.

The matched provider benchmark uses 32 alternating capped/uncapped pairs per
format, with a loopback completion fixture and the real host, Go owners and bus.
Before and after runs use the same harness; the owned CT runs only the relevant
T3 stack during each measurement. All 200 correctness checks pass again in each
cohort. Every timed request dispatches once and preserves its complete baseline
provider body. [Raw samples and CPU measurements](memory-shared-reliability-2026-09-20/admission-poll-timings-f110e9b873.json)
bind the harness hash, application revision and image identity.

| Provider format | Before capped median / P95 | After capped median / P95 | Paired median admission overhead, before → after |
|---|---:|---:|---:|
| OpenAI Chat | 79.11 / 98.09 ms | 75.87 / 98.57 ms | 7.67 → 2.19 ms |
| Anthropic Messages | 80.35 / 100.94 ms | 76.28 / 95.85 ms | 9.83 → 2.70 ms |

P95 uses nearest rank. Paired overhead is the median of each capped latency minus
its adjacent uncapped latency, not the difference between the two cohort medians.
The samples show substantially less median admission overhead. Anthropic capped
P95 improves by about 5%; OpenAI capped P95 is essentially flat. Neither the small
sample nor the fixture establishes the full MR-18 retrieval-performance gate.

The benchmark also samples the economizer process's user/system CPU ticks over
10 seconds without issuing requests. Idle CPU rises from **0.03 to 0.16 CPU
seconds** over that interval: **0.30% → 1.60% of one core**, measured with a 100 Hz
tick counter. The shorter poll ceiling has a real, bounded idle CPU cost. Only
this latency-sensitive process changes; other modules keep the 10ms default.
The C bus and its polling remain unchanged. An event-driven wakeup would require
a separate transport design and is not silently introduced here.

Raw receipts remain under `/opt/aimee-memory-proposals-evidence/t2-f110e9b873`,
`t3-f110e9b873`, `budget-poll-before-140a546a15.json` and
`budget-poll-after-f110e9b873.json`. All nine candidate containers are stopped;
volumes and evidence are retained. The old baseline containers were removed after
their controlled rerun, retaining their volumes and receipts. Local Go race,
native/Go conformance and all 77 lint checks pass. Full CI for the final evidence
commit remains pending. No proposal is newly certified complete by this change.
