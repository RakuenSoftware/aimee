# Embedder sweep

The sweep compares embedding models on LoCoMo and LongMemEval direct-retrieval tests. Candidates
read text on stdin and write a JSON float array on stdout, so the harness is hardware- and
provider-neutral.

**The harness does not run today.** The Aimee direct entry points it calls were removed, and
`benchmarks/embedder-sweep.sh` says so in its own header. The Go replacement is specified in
[dataset benchmark direct track](proposals/pending/dataset-benchmark-direct-track.md). What follows
describes the shape the sweep had and the shape its replacement should keep.

## How it was run

```bash
cp benchmarks/embedder-candidates.txt.example benchmarks/embedder-candidates.txt
# edit the commands
./benchmarks/embedder-sweep.sh
```

For a short smoke run:

```bash
./benchmarks/embedder-sweep.sh --max-samples 50 --max-cases 50
```

Candidate file:

```text
baseline  python3 scripts/embed.py --model all-MiniLM-L6-v2
mpnet     python3 scripts/embed.py --model all-mpnet-base-v2
bge_small python3 scripts/embed.py --model BAAI/bge-small-en-v1.5
```

Keep names short and filesystem-safe. Results land in
`benchmarks/results/embedder-sweep/`; the command also prints and saves a timestamped summary.

## What it measures

For each candidate the harness runs the LoCoMo and LongMemEval direct-retrieval adapters, then
validates each result with `benchmarks/verify_scores.py`.

No answer-generation model runs. The result covers retrieval quality and latency: Recall@K, MRR,
ingest time, and query time.

The embedder is the `embedder_command` config key:

```bash
aimee config set embedder_command "python3 scripts/embed.py --model all-mpnet-base-v2"
```

The shipped script exports `AIMEE_EMBEDDING_COMMAND` per candidate instead, and nothing in the tree
reads that variable. Had the entry points survived, every candidate row would have measured the one
configured embedder and reported it under a different name each time. A replacement must set the
config key per candidate and restore it afterwards.

## Selection bar

Change the default only when a candidate:

- improves Recall@5 or MRR by at least one percentage point on both corpora;
- stays inside the latency budget in [Benchmarks](BENCHMARKS.md);
- has an acceptable license and deployment footprint;
- fits the current pgvector layout, or ships with a versioned re-embed and atomic index cutover.

If the vector dimension changes, build a new index version, backfill it, then switch reads. Keep the
old index until the new one passes. If no candidate clears the bar, keep the baseline and record why.

Record the date, hardware, model, dimension, quantization, license, and command with committed
results. Runs from different hardware are not directly comparable.
