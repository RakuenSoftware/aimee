# Provider browser acceptance

`browser.cjs` drives the built GUI using Chromium. Point it at a dedicated test
installation: it creates two providers and their models, edits them, probes
models, and ultimately deletes the records bearing `PROVIDER_E2E_PREFIX`.
The default prefix is `gui-e2e`. It refuses to overwrite existing records with
those names.

The only substituted production dependency is the external model vendor.
`fixture.py` serves deterministic model listings and completions and records
which test account authenticated each request. The web proxy, native server,
configuration, Vault, and PostgreSQL must be the real deployed build. Never
use this fixture for production inference.

On a fresh Debian test CT/VM, deploy the branch's server, generated process-module
bundle, PostgreSQL, runtime-web, and `frontend/dist/index.html`. Use the normal
PAM login setup. Run the fixture in that guest:

```sh
python3 scripts/validation/providers/fixture.py --port 18765
```

Install Playwright on the browser runner (this need not be the guest):

```sh
npm --prefix /tmp/aimee-provider-browser install --no-package-lock playwright@1.55.0
/tmp/aimee-provider-browser/node_modules/.bin/playwright install --with-deps chromium
```

Supply the GUI URL, isolated PAM test account, and guest-local fixture endpoint.
`PROVIDER_E2E_FIXTURE` is resolved by the native server, not by the browser.
Keep the password out of command lines, reports, and shell tracing.

```sh
export NODE_PATH=/tmp/aimee-provider-browser/node_modules
export PROVIDER_E2E_URL=https://TEST_GUEST:8443
export PROVIDER_E2E_FIXTURE=http://127.0.0.1:18765
export PROVIDER_E2E_USER=provider-tester
read -rs PROVIDER_E2E_PASSWORD
export PROVIDER_E2E_PASSWORD
export PROVIDER_E2E_ARTIFACTS=/tmp/aimee-provider-acceptance
node scripts/validation/providers/browser.cjs
```

The first phase deliberately retains its providers/models. Restart the **real
server and runtime-web**, wait for readiness, then run:

```sh
PROVIDER_E2E_PHASE=after-restart node scripts/validation/providers/browser.cjs
```

Retain both phase reports, screenshots, service revision/PIDs before and after
restart, and the fixture's `/events` output. Verify that the native roster file
contains no literal `fixture-key-*` values. Events expose account labels and
whether authentication succeeded; they never contain authorization headers.
Check that account A uses the rotated key after editing and restarting, while
account B continues using its original key and endpoint.

The suite covers browser login; multiple accounts at one URL without models;
cancelled creates; duplicate names; model discovery and probing; limits and
price editing; blank versus declared-zero prices; endpoint changes; blank-key
preservation; credential rotation; cancelled deletion; small-screen layout;
hard refresh; restart persistence; cascade deletion; and retaining a provider
after its last model is removed.

Screenshots require visual review. Report additional exploratory findings and
any missing environmental coverage alongside the automated results. A local
UI rehearsal using substituted `/api` responses must set
`PROVIDER_E2E_UI_SMOKE=1`; its reports are explicitly marked as UI smoke tests
and **do not count as real-stack, Vault, inference, or restart validation**.

Run `exploratory.cjs` against the same deployment to check cross-origin rejection,
unavailable model discovery with manual entry, incompatible provider edits,
blank-key preservation, and deletion followed by keyless name reuse. A failed
run may leave only its `<prefix>-negative` record; `PROVIDER_E2E_RESET=1` explicitly
removes that record before repeating the exploratory phase.

For process isolation, kill only the providers module in the disposable guest
while the server stays up, then run
`PROVIDER_E2E_PHASE=module-down node scripts/validation/providers/exploratory.cjs`.
Restart the real server/module/web services and run the after-restart phase.
Wait for the web listener before opening Chromium. The dedicated config module
has its own executable: never replace it with the multicall binary.

Build all artifacts from the explicit feature worktree. Retain deployed binary
SHA-256 values with the reports. Remove the disposable guest when validation
is complete; do not repurpose or stop unrelated guests on the hypervisor.
