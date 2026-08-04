# SUPERSEDED — gemma-3n-E4B 10k arm measured at nproc=4

Complete, clean arm: strict 0.5429, 10,000 rows, 0 errored, 0 truncated,
parse_ok 9991. Nothing wrong with it as a measurement.

Superseded because every other arm it would be ranked against ran at nproc=3 --
granite-4.0-1b, granite-4.1-3b, Qwen3-1.7B and the entire E2B/E4B 10k ladder.
Process count is results-affecting and worth 0.0105 F1 (finding 19), which is
larger than several of the gaps this ranking is meant to resolve.

Remains valid for within-arm inspection and for any future nproc=4 comparison.
It is not comparable to the arms at 3. Re-run under the same label at NPROC=3.
