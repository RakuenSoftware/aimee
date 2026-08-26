# Agent-facing code-intelligence red baseline

- **Captured:** 2026-07-29 UTC
- **Aimee proposal:**
  [`agent-facing-code-intelligence-effectiveness`](../proposals/done/agent-facing-code-intelligence-effectiveness.md)
- **Harness checkout:** `/home/virant/dev/ponytail-codex-benchmark`
- **Harness branch / commit:** `agent/codex-benchmark-matrix` /
  `a40fe3d6a58ac4e8b14aaf75320e83912f0bfc56`
- **Run status:** interrupted and incomplete; 97 of 600 expected cells
- **Purpose:** untreated diagnostic controls for E1–E6, not a comparative product claim

## Environment and provenance

The run used Codex CLI `0.145.0`, model `gpt-5.6-sol` at medium reasoning, Ponytail `4.8.4` at
`16f29800fd2681bdf24f3eb4ccffe38be3baec6b`, Aimee client
`v0.2.195-1003-g627c1ffc`, Aimee server `testing-8bc6aa5`, and KB
`vtesting-8bc6aa5`. The fixture seed commit was
`40af4eebffc6b2fe4b6073cecdf3ad1b744607d2`.

The result checkout was intentionally left uncommitted when the KB failed. The complete source
artifacts remain at:

- results: `/home/virant/dev/ponytail-codex-benchmark/battery/codex_results/`, 986 files;
- raw Codex streams: `/tmp/ptcodex/raw/`, 194 files; and
- representative readiness:
  `battery/codex_results/cells/aimee__t06_semver__r1/aimee-readiness.json`.

Checksums bind this record to that mutable local evidence:

| artifact | sha256 |
| --- | --- |
| sorted results-file sha256 manifest | `93f92f6697263c54696318f566b3b19ee5732de332f7a9a0ebdd199d12736a9b` |
| sorted raw-file sha256 manifest | `0441626841c736f4be2c404996ec4a734b5220d9491fbdba5d8cc46b79336133` |
| `provenance.json` | `e7be51c1c31a43feeb060fc4c49a1cca3fe97f52c648d612f6daea0f017e0b93` |
| `red-validation.json` | `1e724d893003924fef54063c3640596baac1add0808eb58fe6e954289fd69d5d` |
| `score.json` | `ec7325e4bfb0197517d11bdad2b7837a70f3ee6e4ca8c3b8631bc1e26180f675` |
| representative readiness | `a5dd73c96bec0df3dae6f0eb7d5c44d9867fdf9089f0b036deee8755ed9ce8f3` |
| root-cause canary raw stream | `9820b7f90b34df7c226e36649a37debd21ba78eb92323a01ee487bcfdf3c0418` |
| topological-sort raw stream | `277ad99445419c3511bad01fa015876b0d87f967d28dd875130d058de9e1523b` |

Recompute the tree-manifest hashes from the harness checkout:

```bash
find battery/codex_results -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum
find /tmp/ptcodex/raw -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum
```

E0 also preserves the minimal non-secret evidence needed to audit every red fixture under
`benchmarks/code-agent-effectiveness/evidence/`. Those tracked extracts record their original path
and checksum, while `fixtures.json` pins the extract bytes themselves. The outage evidence explicitly
labels itself as reconstructed from the live status probe plus the persisted runner checkpoint;
terminal formatting was not captured byte-for-byte. The tracked evidence, rather than continued
availability of the machine-local originals, is the durable control consumed by later slices.

## Untreated observations

### Product behavior

1. **Tool discovery:** the installed skill named `preview_blast_radius`; in
   `aimee__t50_toposort__r1.jsonl`, `find_tools({"query":"blast radius"})` returned zero tools.
2. **Project scope:** `find_symbol("install_order")` in that same stream returned six identical
   definitions across six project namespaces, including but not limited to the active project.
3. **Python blast radius:** the representative readiness report resolved `app/dates.py` and found
   callers in `app/billing.py`, `app/invoices.py`, and `app/reports.py`, but returned empty
   `dependencies` and `dependents`.
4. **Retrieval abstention:** the root-cause canary asked for billing/month-length conventions and
   received four delegation/roundtable episodes, none relevant to the active project. The other nine
   completed repository-specific memory searches showed the same result class.
5. **Availability:** after 97 saved cells, `aimee-server` remained healthy while the KB endpoint was
   unreachable. The runner stopped and preserved artifacts, but the product did not expose the full
   typed unavailable/empty/stale/abstained contract proposed for E5a.

### Adoption and outcome boundary

Across 23 task cells paired between baseline and Aimee after excluding the setup canary, 13 Aimee
cells made no Aimee MCP call. The coding agents made no semantic/hybrid code query, no caller query,
and no successful blast preview; only one cell called `find_symbol`. Thirty-five embeddings were
prepared per isolated fixture, but only readiness queried them.

The paired task score was baseline 20/23 and Aimee 21/23. That one-task difference is not attributable
to indexing: the two Aimee-only wins made no Aimee MCP call. MCP-using cells were costlier and slower
in this small endogenous sample, but task mix confounds the comparison. These observations motivate
the paired standard/observe/on/capability-ceiling design; they do not establish causal harm or lift.

The traversal task's expected duplicate implementation and the audit task's undocumented event
vocabulary are benchmark-validity exclusions. They are preserved in raw artifacts but are not Aimee
product-fix targets.

## Fixture and verification

The durable extracted contract is
[`benchmarks/code-agent-effectiveness/fixtures.json`](../../benchmarks/code-agent-effectiveness/fixtures.json).
It separates `red_observation` from `green_contract` so later treatment cannot rewrite the control.

Run the hermetic schema/integrity check:

```bash
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py --verify-sources
python3 -m unittest benchmarks.tests.test_agent_code_intelligence_fixtures
```

The check does not claim a live product fix. E1–E5 must each replay their relevant case against stock
and treatment builds, and E6 must run a fresh, pinned merged Aimee arm rather than append cells to
this interrupted snapshot.
