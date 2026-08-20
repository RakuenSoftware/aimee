# Proposal: `/v1/server/health` reports the store from a heartbeat, so it is
# wrong for about half a minute after the module dies

- **State:** RESOLVED, by the second option below. Found by running the migrated
  build on a clean container, reproduced deliberately and timed at 36.5s and
  37s; measured at **1s** after the fix, on the same container, same method.

## What happens

Kill the DB1 module while the daemon is running. Every store-backed call starts
failing immediately -- correctly, with `failed to create session`. But
`/v1/server/health` keeps answering:

    {"status":"ok","uptime":...,"state":"ok",...}

Measured on a clean Debian 13 container, twice: the state flipped to
`unavailable` at **36.5s** and **37s** after the module process was confirmed
gone (`pgrep` returning zero matches, not merely a signal sent).

So for roughly the first 37 seconds of an outage the daemon tells anything that
asks -- an operator, a load balancer, a container healthcheck, `aimee status` --
that the store is fine, while it is refusing every call that needs it.

## Why

`db1_store_ready()` asks `obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS)`,
which resolves to `bus_host_kind_has_server()`. That returns whether the slot
registered to serve the kind is still `in_use`:

    return (uint32_t)kind->server < h->cfg.max_slots && h->slots[kind->server].in_use;

Nothing marks the slot free when the module exits. A slot is released in two
places: when a replacement with the same principal attaches (`runtime_bind`
reaps the predecessor via its pidfd), or when the heartbeat reaper notices the
client's heartbeat has stopped advancing. The reaper uses
`stale_after_ns = 30s` (`obs_bus.c`) and runs no more often than
`stale_after_ns / 4` -- every 7.5s. 30 + 7.5 is the 37s that was measured.

This is the bus's designed liveness model and it is shared by every module, not
something DB1 does specially. The predicate that gates *calls*
(`module_json_call`) is the same one, which is why calls in the window are
attempted, routed at a dead slot, and fail rather than being refused up front.

## Why it is worth a decision rather than a shrug

The readiness question was already got wrong once in this migration, in the
other direction: health reported the store from `db1_is_initialized()`, which
became permanently false the moment the daemon stopped opening the database.
That was fixed by deriving health from module availability, and an integration
assertion was added so it stays fixed.

That fix is right about "is anything serving the store" and wrong about "can the
store be used *now*", and those differ for the 37 seconds that matter most --
the beginning of an outage, which is exactly when someone is looking at health.
A healthcheck that is green for the first half-minute of every store outage is a
healthcheck that cannot trigger a restart, and a container that restarts on
health will not restart.

## Options

**Leave it.** It is bounded, it is uniform across modules, and the window is
shorter than most orchestrators' failure thresholds anyway. Costs nothing and
documents a known 37s blind spot.

**Make health probe rather than infer.** Have the health handler issue a cheap
real operation to the module and cache the verdict for a second or two. The
answer becomes true within the cache window instead of within 37s, and health
stops being able to disagree with the calls beside it. The cost is a module
round trip on an endpoint that is polled, which the cache bounds, plus a health
endpoint that can now be slow when the module is slow -- arguably a feature.

**Shorten the stale window.** One constant, applies to every module. It makes
every module's liveness sharper and makes false reaps of a briefly-stalled
module more likely. This is a bus-wide policy change and should not be made to
fix one endpoint.

## What was done

The second, scoped to the health endpoint only, in `src/db1_store_probe.c`.

`db1_store_ready()` is unchanged and still means "is a module serving the
store". It has to stay cheap: it guards every store-backed command, on the way
to a call that reports its own failure anyway. Making *that* probe would put a
round trip in front of every command to fix one endpoint.

`db1_store_probe()` is the new one, and only `handle_server_health` calls it. It
asks the store a real question -- `server_session_count` over a window nothing
can fall into, so it reads an index and returns zero rather than counting rows
on a busy store -- and caches the verdict for one second. The cache is what
stops a polled endpoint from becoming load on the module; a second is short
enough that no operator or orchestrator could act on the difference. If nothing
is registered at all it answers from the registry without a call, so a daemon
that never had a store does not pay a timeout to be told so.

The round trip happens outside the lock. Two concurrent probes cost one extra
call, which is cheaper than serialising every health request behind the
module's latency.

The third option -- shortening the bus's stale window -- was not taken, and that
reasoning stands: it is one constant shared by nineteen kinds, and changing
liveness semantics for every module to fix one endpoint trades a known bounded
staleness for unknown false reaps.

`test_integration.sh` now kills the module and asserts health stops saying "ok"
within five seconds. A regression to inferring availability fails it by twenty
seconds rather than by a hair, which is the margin worth having.

## How to reproduce

`scripts/validation/db1-module-readiness-probe.sh`, run inside a container with
the server and module installed. It starts both, proves a store call works,
kills the module, proves the process is gone and nothing replaced it, then polls
health every half second and prints when it flips.
