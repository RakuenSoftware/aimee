# Memory browser regression

Run `browser.cjs` with Playwright against an isolated appliance containing a personal record
and a KB record with the same numeric ID. The test authenticates through PAM, switches stores,
retires the personal record, verifies the KB record stays active, and captures desktop/mobile
screenshots. It changes fixture data.

Set `MEMORY_E2E_URL`, `MEMORY_E2E_USER`, `MEMORY_E2E_PASSWORD`, `MEMORY_E2E_LOCAL_KEY`,
`MEMORY_E2E_KB_KEY`, and optionally `MEMORY_E2E_OUTPUT`, then run:

```sh
NODE_PATH=/path/to/playwright/node_modules node scripts/validation/memory/browser.cjs
```

The API/CLI/MCP and outage regression is `tests/e2e/memory-placement-e2e.py`. Its required
container arguments and output path are listed by `--help`. Docker CI runs it in its fresh
split-stack topology; do not point either script at a user's installation.


## Legacy catalog migration

Provider-specific deployment labs and sidecar contract runners were retired with
the Cognee/Hillock serving paths. Dated reports retain their historical results.
Build the private `integrations/legacy-memory-transfer` utility in aims-db to read
existing catalogs. `native-transfer.py --validator /absolute/aimee-memory-transfer`
continues to validate the same offline interchange, including retained erasure
controls. This is not an automatic import into aims-db.

The backend-neutral `qualify-conversation.py`, `answer-quality.py` and
`model-request.py` remain available for disposable current native fixtures.
Current aims-db paired and extraction experiments live with its private product.

## Extracted evaluation and seed tools

The public build no longer builds or installs `aimee-memory-eval`, the concrete
ontology seed command, or the private transfer executable. In a separate private
aims-db checkout, `experiments/legacy-aimee-go/qualify.py --help` describes the
immutable legacy evaluator/seed qualification. The moved three generator
regressions and pinned native-table fixture live in `generator-fixtures/` there.
The current public serving-contract gate is `scripts/check_current_memory_contracts.py`;
it explicitly excludes the ten extracted evaluator cases and does not claim full
historical MR18 certification. `native-transfer.py` retains its explicit
`--validator` override (and may use a separately installed private validator).
