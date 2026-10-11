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


## Corpus size and conversational qualification

From `server-go`, run `go run ../scripts/validation/memory/qualify-external.go`
with `-backend catalog|hillock|cognee`, `-sizes 100,1000,10000`, and an output path.
External providers require an isolated endpoint and private token file. Cognee
uses `-auth api-key`. The workload imports synthetic attributed records, reopens
the canonical owner, preserves an old lexical needle, and measures cold and
concurrent warm recall including the shipping handler's serial queue. Each request
has a deadline. Provider metrics identify HTTP routes, never credentials or bodies.
This single-needle workload establishes latency and identity, not semantic recall
coverage over an arbitrary corpus.

`lxc/conversation-quality.py` freezes eight scoped synthetic multi-speaker fixtures
through the shipping API. `answer-quality.py` evaluates the frozen records against
an OpenAI-compatible real model using JSON argv supplied by `--model-command`.
The model command takes a JSON request on stdin and emits the response on stdout;
no shell expansion is performed by the harness. Results retain outputs, usage,
omissions and literal acceptance terms for review. Phrase matching is a limited
acceptance check; inspect the answers as well.

`lxc/bridge-profile-probe.py` imports the shipped Discord conversation adapter and
checks episode authorship, restart reconstruction, exact units, canonical mutation
blocking during a send guard, explicit completion and agent reply provenance.
It does not send messages or test Discord gateway behavior.

`run-cognee-contract.py` defaults to deterministic local model fixtures for CI.
To qualify real inference and embeddings, provide both `--completion-command` and
`--embedding-command` JSON argv plus their model names and embedding dimensions.
The `model-request.py` gateway supports an existing mTLS OpenAI model and Aimee's
native embedding service. Native embedding conversion uses the document prefix;
use it for a shared query/document space only when `/health` confirms identical
prefixes (as in the qualified Bekko fixture). A compatible OpenAI wire alias can be
needed by Cognee's client; record the actual serving identity separately.

Use a fresh artifact directory for every provider contract run. Never use these
mutation probes with a user's store or export private resume/config files.


## Hillock candidate scenarios

`run-hillock-contract.py --revision <full-sha> --branch rc/v<major>.<minor>.<patch>
--scale --artifacts <empty-directory>` fetches and verifies that exact upstream
commit, runs the real engine through Aimee's adapter and owner/public-command/recall
scenarios, then repeats after restarting Hillock. Required live tests cannot skip.
The scale workload checks cold and concurrent warm retrieval at 100/1,000/10,000
records. See the [scenario inventory and first-start setup](../../../integrations/hillock/README.md).
These tests qualify Aimee's HDC/subword integration; they do not qualify Hillock's
chat/graph/learning features or arbitrary conversational answer quality.
