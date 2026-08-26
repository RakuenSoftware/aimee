# Model store: fetch once, serve from disk

`smoothnas` (.254) exports `/mnt/media/storage` over SMB and NFS with ~43 TB
free. GGUFs live at:

    /mnt/media/storage/models/gguf/<repo-basename>-<QUANT>.gguf

e.g. `gemma-4-E2B-it-UD-Q6_K_XL.gguf`. Flat, no HuggingFace cache layout, because
the layout is what makes a cached file hard to reuse from another machine.

## Why this exists

One afternoon of benchmarking downloaded roughly 30 GB and then threw most of it
away:

- `prune_models.sh` deletes every quant except the one under test, so a six-arm
  sweep re-downloads six times. It was added because `.253` has a 240 GB disk and
  a full quant ladder fills it, a correct fix for the wrong layer.
- Each host keeps its own `HF_HOME`, so `.254` having a model does nothing for
  `.253`.
- The E4B arms were unavailable later the same day because the earlier sweep had
  pruned them.
- Moving E2B Q6 from `.254` to `.253` needed a 4.7 GB copy over two SSH hops,
  purely because there was no shared location.

None of that is a bandwidth problem. It is a placement problem.

## Rules

1. **Fetch into the store, not onto a host.** Download straight to
   `/mnt/media/storage/models/gguf/`. A host pulling from the store is a LAN copy
   at wire speed; a host pulling from HuggingFace is a WAN transfer that can fail
   or throttle.
2. **Serve with `-m <path>`, never `-hf <repo>`.** `-hf` re-resolves against the
   Hub at start-up, which reintroduces the network dependency the store exists to
   remove, and it also pulls the `mmproj` projector unless `--no-mmproj` is given.
3. **Verify by size before use.** Copies are byte-exact or they are wrong. The
   Q6 transfer was checked at 4,710,088,800 bytes on both ends.
4. **Never prune the store.** `prune_models.sh` may keep clearing a host's local
   scratch; the store is the durable copy and re-fetching from it is cheap.

## Getting a model onto a bench host

    # from the store, over the LAN
    ssh <host> 'curl -s -o /opt/hf/<name>.gguf http://192.168.1.254/models/gguf/<name>.gguf'

or mount the share read-only and point `-m` straight at it, which avoids the copy
entirely for hosts with a fast link.

## Contents

| file | bytes | notes |
| --- | ---: | --- |
| gemma-4-E2B-it-UD-Q6_K_XL.gguf | 4,710,088,800 | current default quant |
| gemma-4-E2B-it-UD-Q4_K_XL.gguf | ~3.0 GB | quant-ladder comparison |

E4B UD-Q4/Q6/Q8 and the larger ladder models should be added the next time they
are fetched, rather than pruned again.
