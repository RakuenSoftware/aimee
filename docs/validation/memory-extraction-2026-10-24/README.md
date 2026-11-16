# First memory implementation extraction

Branch: `refactor/extract-memory-to-aims-db`, based on Aimee testing
`35418e47e13a1d690e402a093d72002c0e1ad7b3`. No installed service or user data was
changed. This branch retires superseded implementations while retaining current
native command coverage.

## Removed and moved

- Retired in-process Cognee/Hillock serving adapters, provider sidecars, independently
  served compatibility catalog, provider-specific egress routes/credential release,
  public catalog commands and their obsolete deployment/prerelease runners.
- Moved the catalog implementation and unchanged offline transfer CLI into private
  aims-db `integrations/legacy-memory-transfer`. It preserves existing data, exact
  revisions, history and retained erasure authority. It is offline migration tooling,
  not another active journal. Native-transfer.py accepts its executable via
  `--validator`. Conversion into aims-db is a separate explicit mapping.
- Removed four dead C headers and four already-unbuilt C suites. These fixtures
  were not executed before removal; their retirement does not improve test coverage.
  [The C retirement review](../memory-c-retirement-2026-10-24.md) identifies active
  regression owners and the mixed orphan specifications deliberately retained.
- Removed duplicate frontend endpoint validation and retired setup choices. Old
  saved selections remain visible and block readiness until explicitly replaced.
  Retired/unknown selections fail startup, including environment overrides; there
  is no fallback or automatic data/secret destruction.
- Removed egress's dependency on the memory-backend config snapshot and its unused
  request-only config principal. Embedding, provider and generic MCP egress remain.
  The installed Vault gate now exercises actual MCP transport and credential custody
  instead of depending on a retired memory provider. No production policy exception
  was added for its disposable fixture.

The private Rust frontend separately consolidates authoritative record scope
projection and exact-version reads across shared/personal/DataJSON paths: 62 net
Rust lines removed across ten files, with no added RPCs or journal reads.

## Actual owner boundary

The main Go memory owner goes from 238 implementation files / 50,131 physical
lines to 231 / 47,666, and from 201 test files / 44,942 lines to 194 / 42,868.
Those figures exclude other C/UI/tooling cleanup. This removes 2,465 implementation
lines and 2,074 test lines from that owner directory; it does not claim the entire
50k-line owner has been replaced. The private migration tool preserves the narrow
legacy catalog subset rather than discarding authoritative records.

The current Go routing table still includes 119 commands. The private Rust frontend's
six original public verbs plus explicit processing commands do not establish
compatibility for all of those workflows. Keep the live owner until each required
command is implemented or deliberately retired with its callers. The generic
supervisor already accepts independent executables; remaining process-contract,
packaging and private-command compatibility must be qualified before changing the
installed memory executable.

## Further extraction targets

| Area | Intended owner and prerequisite |
| --- | --- |
| Version/temporal eligibility, source lineage, correction and ontology structure | Reuse generic aims-db mechanisms; finish each named-query/mutation wire adapter and its exact-current/historical source controls before deleting Go paths. |
| Concrete ontology, protected memory categories, ranking and review workflows | Private aimee-memory policy over generic DB structures; retain original expression and Model authorship. |
| C KB insertion screening/redaction | Move concrete admission policy to the private owner only after preserving exact original content, refusal/redaction options, PII scope and insertion audit through actual callers. Current active screening remains. |
| Code indexing and learned retrieval workflows | Separate reusable indexing/storage behavior from application learning and execution feedback; no blanket deletion of those consumers. |
| Mixed unbuilt C context/evaluation specifications | Split and port the still relevant host/benchmark assertions before removing their obsolete memory portions. |

## Validation and limits

- [Fresh restricted-role PostgreSQL owner evidence](owner/README.md): 1,244 passing
  test/subtest events, zero failures, eight passing packages. The initial missing
  C conformance-host skip is retained and closed by a separately built strict C
  host plus passing race-enabled Go client test. Source hashes stayed unchanged;
  the owned fresh cluster was stopped.
- Frontend TypeScript and production Vite build pass, 25 unit tests and nine actual
  Chromium scenarios pass. C setup fixture built with Werror and passed.
- 16 C-boundary and seven runner-inventory tests pass; the active runner count stays
  676. Memory ownership, module inventory/ownership, source snapshot, process
  contracts, runtime-bundle checks, module docs and diff checks pass. Ownership
  checker tests pass six; core contract tests pass 27; source-lock tests pass seven.
- The broad Go peer-import checker remains failing baseline debt: 110 findings
  before cleanup, 102 after, zero introduced. This branch does not weaken it or
  rewrite external repository provenance pins. Current bundled-source hashes are
  updated through the application source snapshot.
- The MCP Vault fixture probe compiles and its three offline protocol tests pass.
  The actual installed-image Docker/hardening gate is retained and required in CI,
  but was not run locally because Docker is unavailable. Fixture checks are not a
  substitute for that gate. No image/deployment certification is claimed.
- Private frontend checks pass all 52 tests, strict Clippy and formatting. The
  relocated migration utility passes eight race-enabled tests, vet, and standalone
  readonly tests after authenticated pinned-source acquisition; dependency sums are
  committed and no local checkout replacements are required.

[Check logs and hashes](checks/manifest.json) supplement the raw owner evidence.
Earlier dated sidecar results remain historical evidence of their named snapshots.
