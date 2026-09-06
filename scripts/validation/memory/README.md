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
