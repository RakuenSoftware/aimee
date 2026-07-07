# Proposal: User-selectable retrieval fusion mode + a benchmark users run to choose it

- **State:** proposed (pending — not started)

## Thesis

There is no universally-best retrieval fusion mode. `rrf` is a safe rank-blend;
`static_alpha` fixes a lexical/dense weight; `dynamic_alpha` predicts that weight
per query (high alpha → boost the lexical leg for identifier/exact-token queries,
low alpha → favour dense for prose). Which wins is **corpus- and query-mix-
dependent** — dynamic alpha helps identifier lookups and can hurt purely semantic
questions. The right design is therefore not "pick a default and flip it," but:
make the mode **user-selectable**, and give the user a **benchmark they run on
their own data** to see the tradeoff and choose — including choosing per-scope
defaults. The engine for that benchmark now exists (`benchmarks/kb/dynamic-alpha/
run.py`); this proposal wires it into a product surface.

## Goal

An operator can (1) switch the KB fusion mode among `rrf` / `static_alpha` /
`dynamic_alpha` from the GUI, (2) run a fusion A/B benchmark against their own KB
and query set from the same surface, and (3) see a per-shape better/worse split
that recommends a default — set with one click. No code change to try a mode; no
guesswork to pick one.

## §0 What already exists

- **All three modes are implemented + applied.** `kb_search_fused` (`kb.c:1535`)
  honours a `fusion_mode_override` and falls back to config `kb_fusion_mode`;
  `POST /v1/search` (`kb_http.c:1048`) reads `fusion_mode` from the request and
  returns `fusion_mode_used`. Verified live: `rrf` vs `dynamic_alpha` produce
  different scoring. `dynamic_alpha`'s `predict_alpha` is unit-tested
  (`unit-test-kb-fusion`).
- **Config field exists.** `kb_fusion_mode` (default `rrf`) + `kb_fusion_static_alpha`
  in `config_learning.c`; `config.set` persists `aimee.yaml`.
- **Benchmark engine exists** (this PR): `run.py` runs any labelled query set in
  all modes against a live KB and reports the per-shape better/worse split.
- **Settings surface exists but is typed bool/int only.** `webchat/settings.go`
  (`settingsAllow`) + `/api/settings` + `/api/config/set` → `/v1/config/set`.

The pieces are present; they are not connected into a selectable, measurable
surface.

## §1 Selectable mode (all three) in the GUI

Extend the webchat settings model with an `enum` field type (options + current
value); render it as a `<select>`. Add `kb_fusion_mode` as an allowlisted enum
(`rrf` / `static_alpha` / `dynamic_alpha`). `config.set` already persists it, and
the search default already reads it — so selection takes effect on the next query
with no restart. Expose `kb_fusion_static_alpha` as a companion numeric when
`static_alpha` is chosen.

## §2 User-facing benchmark

Surface the engine two ways:

- **CLI:** `aimee kb fusion-bench [--fixtures FILE] [--k N]` — routes to the KB,
  runs the modes, prints the report. `--fixtures` defaults to the charter set but
  accepts the operator's own labelled queries (the format is a small JSON: query +
  `expected_top_path_substring` + `shape`), so it runs on **their** corpus.
- **GUI:** a "Tune retrieval" panel that runs the same benchmark against the
  active KB and renders the per-shape table + aggregate P@k, with the operator's
  saved query set (editable in the panel).

## §3 Data-driven default (and per-scope defaults)

The benchmark already computes which mode wins by shape; add a recommendation:
"dynamic_alpha wins N positives, regresses 0 guards → recommend as default" (or
the converse). Offer a one-click "set as default." Because DB2 knowledge is
project/scope-partitioned, allow a per-scope override (`kb_fusion_mode:<scope>`)
so a code-heavy project can run `dynamic_alpha` while a prose corpus stays `rrf`.

## §4 Precondition: corpus health + a code-search variant

Two honest gates (documented in the benchmark README):

- The benchmark is only meaningful against a KB whose curator/doc-embed drain has
  fully drained — on a degraded corpus every query misses in every mode
  (inconclusive, not evidence). Gate the GUI panel on a corpus-health check.
- `/v1/search` ranks `doc_chunk`s; identifier→code fixtures need a code-search
  variant against `/v1/code/*`. Ship the doc benchmark first; add the code variant
  when the code-search path grows a fusion knob.

## Non-goals

Not changing the fusion algorithms, and not auto-flipping a default without the
operator's confirmation — the whole point is to put the choice, and the evidence
for it, in the operator's hands.
