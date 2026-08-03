# Contaminated. Do not use these numbers.

Produced 2026-08-03 08:43-09:37 by `harness/rebuild_quant_ledger.sh`, before the
socket-based kill landed. Every arm after the first is partly answered by the
PREVIOUS arm's weights.

## Mechanism

`kill_servers` built one remote shell command containing a `pkill -f "port N "`
for each port. That shell's own command line contains every one of those
patterns, so the first `pkill` matched the shell and killed it. Port 8400's
server died with it; ports 8401 and up were never reached and survived. The
replacement servers could not bind, `/health` answered from the survivors, and
the driver proceeded.

## Evidence

Shards are round-robin over gold line order, so shard membership is
reconstructible. Raw-output identity between arms that must differ:

| comparison | shard0 | shard1 | shard2 |
| --- | ---: | ---: | ---: |
| gold70 E4B Q6 vs Q8 | 77% | **100%** | |
| gold70 E4B Q4 vs Q6 | 40% | 83% | |
| v5small E2B Q4 vs Q6 | 27% | 85% | 83% |

Shard 1 byte-identical across two quants is only possible from one set of
weights. Shard 0, the only port the broken kill actually cleared, behaves
normally throughout.

## What this invalidated

The reading that E2B Q6-Q4 comes out negative, which was about to be written up
as a replication failure. It was an artifact of two thirds of the Q6 arm running
Q4.

## What is NOT affected, and how that was checked

The same per-shard test on clean runs shows uniform identity across shards:

- `results/10k-sharded/` E4B Q4 vs Q6: 25.5% / 24.8% / 26.1%
- `results/v8-baseline/` (single server per arm): 25-42% between quants

Both stand.
