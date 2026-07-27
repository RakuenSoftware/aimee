#!/usr/bin/env python3
"""Fail closed after the EuroBERT sweep moved off the `.254` production host."""

raise SystemExit(
    "EuroBERT testing moved to the RTX 5080 on .253; "
    "run benchmarks/gemma4_baseline/run_253_eurobert_rerankers.py there"
)
