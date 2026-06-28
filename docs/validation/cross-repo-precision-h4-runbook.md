# H4 — cross-repo precision live re-validation runbook (.254)

The full precision-hardening build is merged to `testing` (PRs #824, #825, #827,
#828, #830, #831, #832, #835 — H0a–H0d, H1, H2, H3a, H3b). H4 deploys it to the
split stack on `.254` (192.168.1.254), repopulates the index-time metadata, and
spot-checks that the known live false-positive classes collapse while the true
dependencies survive.

Decision (this session): ground truth = **spot-check the known FP classes only**
(no formal Wilson-CI N≥100 labeling).

## 0. Access
- `.254` is a CT on pve; this `code` host (192.168.1.99) has no SSH key for it.
  Run the deploy steps from pve (`pct exec <ctid> -- ...`) or with `.254` shell.
- sudo on `.254`: `echo 998Fgh | sudo -S <cmd>`.
- tierd admin: `plugadm.py` at `http://127.0.0.1:8420` (admin / 998Fgh), from `.254`.
- Use tierd lifecycle verbs (not `docker restart`) to bounce plugins.

## 1. Deploy latest `:testing`
`.254` plugins are pinned to the moving `:testing` tag, so they auto-track merges,
but the running containers must pull + update:
1. Pull the new images on `.254`: `sudo docker pull <registry>/aimee-kb:testing`
   and `aimee-server:testing` (the GPU `aimee-llm` image is unchanged — no re-pull).
2. Update + bounce via tierd (plugadm) so the kb + server plugins run the new image.
3. Verify versions: the kb/server should report a build at or after `9c6bd56`.

## 2. Repopulate metadata (the re-scan)
The H0 per-file metadata — `terms.def_kind` (H0a), `files.language` + `files.vendored`
(H0b) — is written at SCAN time (`canonical_index_scan_project`), so a re-scan of
the corpus is required:
1. Re-scan all indexed repos (the `~/dev` + `~/gow` workspaces, ~40 repos) so the
   files/terms rows carry def_kind/language/vendored.
2. The repo-identity index (H0c), the inter-repo route index (H0d), and the
   blocked-symbols distinctiveness model rebuild AUTOMATICALLY: H1 wired a
   one-shot cold-start rebuild into the curator drain (`kb_curator_drain.c`), so a
   kb restart (step 1.2) repopulates `cross_repo_identity` + `cross_repo_route` +
   `blocked_symbols` once the store is ready; the `built>0` incremental keeps them
   fresh as the re-scan lands changed projects. No manual rebuild call needed.
3. Wait for the drain to settle (watch the kb log for
   `kb.cross_repo.meta: rebuilt cross-repo metadata: identities=N routes=M ...`).

## 3. Spot-check (from any host with the enrolled remote, e.g. this one)
The CLI targets `.254` via `~/.config/aimee/remote.conf`. Run:
- `aimee index deps moonlight-qt`  → expect **moonlight-common-c** (HIGH, via
  LiStartConnection / Limelight.h route). The previously-seen FP
  `moonlight-qt → Sunshine` (DEFINE_GUID) must be GONE (no real route; macro
  capped below HIGH; §2/extractor drops `<...>`).
- `aimee index deps wolf`          → expect inputtino / gst-wayland-display /
  moonlight-common-c; no spurious cross-lang edges.
- `aimee index deps smoothnas-plugin-aimee` → expect aimee / smoothnas.
- Reverse: dependents of `moonlight-common-c` include `moonlight-qt`.
- Confirm the specific live FPs from §0 collapsed: `DEFINE_GUID`→Sunshine,
  `authenticate` (generic), cross-language `motion` (C++ vs Rust) — none has a real
  `cross_repo_route`, so each is now LOW-unresolved (not emitted).

## 4. Recall sanity
On `.254`, grep the kb log for `low-unresolved (no route)` (the H1 instrumentation)
to see which would-be edges were dropped for lack of a route — distinguishes the
build/link-only recall loss (CMake `target_link_libraries` with no source include,
which H0d does not model) from genuine no-dependency. If a known true dep is
missing AND appears in that log, it is the link-only class (a follow-on, not a
regression).

## 5. Outcome
- If the known FP classes collapsed and the true deps survive → flip
  `docs/proposals/pending/cross-repo-precision-hardening.md` state to COMPLETE and
  move it to `docs/proposals/accepted/` (or per repo convention).
- Record the before/after for the spot-checked repos here.
- Clean up any temporary CTs/scratch created on pve.

---

## H4 RESULTS (2026-06-28, .254 vtesting-9c6bd56, full corpus re-scanned)

Deployed the complete build (H1–H3b) to `.254`, re-scanned 36/37 repos from the
client (corpus lives on host `code`/192.168.1.99, not in the kb container),
restarted the kb → cold-start rebuild produced **identities=20, routes=82,
blocked_symbols=244**. Spot-check of the known FP classes:

| query | result | verdict |
|---|---|---|
| `moonlight-qt → Sunshine` | **MEDIUM** via DEFINE_GUID (+ cuda.h/input.h/nvhttp.h/vaapi.h routes) | FP — reduced (was HIGH; §5 macro cap worked) but NOT eliminated |
| `wolf → Sunshine` | **HIGH** via buffer_descriptor_t (route: config.h) | FP — persists at HIGH |
| `wolf → inputtino` | HIGH, 13 symbols / 32 sites | TRUE POSITIVE ✓ |
| `moonlight-qt → moonlight-common-c` | **absent** | TRUE POSITIVE MISSING — recall loss |

### Two diagnosed root causes (both NEW work, beyond H1–H3b)

1. **Residual FPs = incidental header-basename routes.** The Sunshine routes are
   shared basenames: moonlight-qt's cuda.h/input.h/nvhttp.h/vaapi.h are the
   caller's OWN headers (verified caller_has_local=t for all 4), and the self-route
   is excluded so the route fans to Sunshine instead. wolf→Sunshine is via
   `config.h` (a BUILD-GENERATED header in only 2 non-vendored repos, so the ≥4
   header-IDF doesn't catch it; wolf's own config.h is generated, not indexed).
   - Fix A (prefer-local header resolution): no cross-repo import_header route when
     the CALLER repo has its own file matching the include basename → kills 5/6
     Sunshine routes (all of moonlight-qt's + wolf's rswrapper.h).
   - Fix B (generated/build-header reject): a small seed set (config.h, version.h,
     …) never forms a cross-repo route → kills wolf→Sunshine.

2. **Recall loss = the angle-bracket blanket-drop.** moonlight-qt depends on
   moonlight-common-c via `#include <Limelight.h>` (angle brackets — an installed
   lib), and the C extractor (`c_import_line`) records ONLY quoted `#include "..."`,
   skipping `<...>`. So `<Limelight.h>` never becomes a file_import → 0 routes
   moonlight-qt→moonlight-common-c → the real edge is dropped. (verified: 0
   Limelight file_imports; moonlight-qt has no local Limelight.h; moonlight-common-c
   defines LiStartConnection; moonlight-qt calls it once.)
   - This revises H3b's "angle-bracket already enforced" conclusion: the blanket
     `<>`-skip is too aggressive. §2's intent was to distinguish SDK `<>` from
     external-lib `<>` via IDF / the repo-identity layer, not drop all `<>`.
   - Fix C (recall, the larger one): capture `<>` includes too (an `is_system`
     flag), and form a route when a `<Foo.h>` resolves to a repo that PROVIDES
     Foo.h (repo-identity / non-ubiquitous), keeping the SDK reject via the
     system-header list + IDF. Needs extractor + file_imports column + re-scan +
     route logic — the H0e/§2 work previously deferred.

### Status
Build (H1–H3b) measurably improved precision (DEFINE_GUID HIGH→MEDIUM;
vendored→canonical; wolf→inputtino clean HIGH) but live validation shows it is NOT
yet sufficient: Sunshine FPs persist and a key true dep is lost to the `<>`-drop.
Next: Fix A+B (precision, ~1 slice) and Fix C (recall, larger). Each → roundtable →
PR → redeploy → re-scan → re-spot-check.

---

## H4 FINAL RESULTS (2026-06-28, .254 vtesting-b35f1f7, full H0–H7 build)

After H5 (prefer-local + generated-header reject), H6 (angle-include capture +
is_system), and H7 (shared system-header list incl. Windows headers), redeployed +
full `--force` re-scan + kb restart (routes=63):

| query | result | verdict |
|---|---|---|
| `aimee` | No cross-repo dependencies | `<process.h>`→Sunshine FP GONE ✅ |
| `moonlight-qt` | → moonlight-common-c HIGH (44 syms / 93 sites) | true dep RECOVERED ✅ (was missing; `<Limelight.h>`) |
| `wolf` | → inputtino HIGH (13 syms / 32 sites) | true dep intact ✅ |
| routes INTO Sunshine | 0 rows | all Sunshine FPs eliminated ✅ |

Before (H1–H3b only): moonlight-qt→Sunshine MEDIUM (FP), wolf→Sunshine HIGH (FP),
moonlight-qt→moonlight-common-c MISSING. After (H1–H7): every known FP collapsed,
the recall loss recovered, true deps intact.

PASS — acceptance bar (this session's choice: spot-check the known FP classes).
The full Wilson-CI N≥100 precision/recall measurement (proposal §9) remains the
formal acceptance for a future pass; the structural mechanisms (§1–§6) are all
implemented + verified-already-satisfied and live-validated on the known cases.

Residual / known limitations (documented, not regressions):
- within-edge per-symbol noise: an emitted edge is repo-level correct, but a few
  of its linking symbols can be coincidental name collisions (e.g. the
  moonlight-qt→moonlight-common-c example symbol shown is a generic one). Edge-level
  precision is the proposal's target; per-symbol pruning is finer-grained future work.
- build/link-only deps (CMake target_link_libraries with no source include) are not
  modelled (H0d builds routes from file_imports); logged as `low-unresolved (no route)`.
- generated/SDK header long-tail: the system-header + generated-header lists are
  seed lists (the principled fix is §1.6 marker-based generated-output attribution).

---

## FORMAL §9 MEASUREMENT (2026-06-28, .254 vtesting-b35f1f7, full corpus)

Enumerated the COMPLETE emitted edge set (deps query over all 40 projects) +
built recall ground truth from intra-corpus `GIT_REPOSITORY` build deps.

### Edge set
**Total emitted edges: 4 (all HIGH, 0 MEDIUM, 0 AMBIGUOUS surfaced).**
| edge | adjudication |
|---|---|
| gst-wayland-display → smithay (167 syms) | TRUE |
| moonlight-qt → moonlight-common-c (44 syms) | TRUE |
| wolf → inputtino (13 syms) | TRUE |
| inputtino → wolf (2 syms, create_touch_screen) | **FALSE** — wrong direction; create_touch_screen is defined in inputtino + used by wolf. Arises from wolf's vendored/duplicate inputtino copies (incl. indexed `.aimee/worktrees/` pollution) |

### §4 precision — NOT MEASURABLE at the required N
Gate: N≥100 HIGH edges, Wilson-95% lower bound ≥90%, AMBIGUOUS in denominator.
Only **4** HIGH edges exist corpus-wide — cannot sample 100. Observed precision
3/4 = 75% (Wilson-95% LB on 3/4 ≈ 30%, but driven entirely by N=4). The gate is
UNREACHABLE because edge VOLUME collapsed, not because precision is poor.

### §5 recall — FAILS
Ground truth (intra-corpus declared deps via GIT_REPOSITORY URLs): 11 pairs;
emitted: 2 → **recall ≈ 18%** (gate: HIGH ≥70%, HIGH+MED ≥85%). Missed real deps
include wolf→{mdns_cpp, moonlight-common-c, gst-wayland-display, eventbus},
smoothnas→{inputtino, smoothfs, eventbus, …}, aimee→smoothgui. (Ground truth is a
lower bound — git-URL scan; submodule/path deps like moonlight-qt→moonlight-common-c
are additional and ARE emitted.)

### §6 latency / negatives
Negatives: of 4 edges, 1 FP (inputtino→wolf). End-to-end deps p50≈812ms /
p95≈1189ms (CLI+TLS+remote round-trip, NOT the resolver-only metric the gate
specifies; the index-time-precompute resolver itself is not isolated here). The
`low-unresolved (no route)` instrumentation is LOG_DEBUG, below the kb's INFO level,
so the no-route demotion volume was not captured.

### VERDICT: §9 deployment gates (#4/#5) NOT met
The precision-hardening eliminated the false-positive FLOOD (P1's ~10–20% precision
on a large edge set) but over-corrected: the edge set collapsed to 4, so precision
can't be measured at N≥100 and recall is ~18% (vs the 70/85% gates). The earlier
spot-check (FP classes collapsed, key true deps recovered) PASSED, but the formal
N≥100/recall bar does NOT.

### Root causes of the recall collapse (for the follow-up)
1. **FetchContent / vendored deps dominate this corpus.** wolf/Sunshine pull
   mdns_cpp, moonlight-common-c, inputtino, gst-wayland-display via CMake
   FetchContent into `_deps/` (vendored); the caller's use resolves to the vendored
   copy, and H2 canonical-preference only routes to the canonical repo when a
   structural route to it exists — which it usually doesn't (the include points at
   the FetchContent path, not a basename that matches the canonical repo).
2. **Build/link-only deps not modelled.** target_link_libraries + find_package +
   FetchContent declare deps with NO matching source `#include` route; H0d builds
   routes from file_imports only (documented gap). These are the bulk of the misses.
3. **Repo-identity layer lacks CMake identities.** cross_repo_identity has only
   crate/npm/pypi (19 rows); no `project()`/target identities for the C/C++ repos,
   so import_module routes can't form for them and §5's package→repo recall mapping
   can't run for CMake deps.
4. **Indexing pollution.** wolf indexes its own `.aimee/worktrees/` copies (other
   sessions' worktrees), duplicating symbols and producing the inputtino→wolf FP.

### Recommended follow-up (recall recovery — a new effort)
- Exclude `.aimee/worktrees/` (and similar) from indexing (cheap; kills the
  inputtino→wolf FP class).
- Build-link route extraction: parse CMake FetchContent_Declare GIT_REPOSITORY +
  target_link_libraries + find_package, and Cargo/go.mod path/git deps, into
  cross_repo_route as an `import_build` route kind (the §1 build/link route source,
  deferred in H0d).
- Populate CMake project()/target identities (the H0c CMake parser appears not to
  emit them on this corpus) so module/target routes form.
- Treat a FetchContent'd dep as a route to the canonical repo (map the _deps/ source
  to its declaring GIT_REPOSITORY → canonical repo).
