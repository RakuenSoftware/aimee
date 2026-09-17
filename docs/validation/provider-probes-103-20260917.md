# Provider probes on .103, 2026-09-17

All four configured HTTP models failed on the installed v0.4.4 image. The three
Codex models returned HTTP 403 and MiniMax returned HTTP 404. The Go probe used
Chat Completions for Codex and omitted `/v1` from MiniMax's path-prefixed
Anthropic endpoint. It also looked up an API key for the OAuth connection, and
the GUI ignored `execution_error`.

The repair follows the serving drivers' wire contracts and Vault slots. Codex
uses `/responses`, OAuth bearer/account headers, `store:false`, streaming input,
and completed final text. Text delivered in `output_item.done`,
`output_text.done`, or `content_part.done` is retained when `response.completed`
has empty output. Failed and truncated streams remain failures. Anthropic URLs
retain exactly one `/v1`. The GUI reads the returned error, and the backend keeps
legacy native-client error, slot, and context field aliases.

## Validation

- `go test -race ./modules/providers ./modules/egress` passed, including a real
  local HTTP request with sealed OAuth credentials and account headers.
- Frontend `Providers.test.tsx`: 9 tests passed, including new and legacy probe
  error display. TypeScript and the production SPA build passed.
- Provider seam, module source descriptor, and whitespace checks passed.
- Real probes on .103 passed before and after container recreation. After
  recreation: Astra 4008 ms, Terra 1325 ms, Luna 1925 ms, MiniMax 2180 ms.
- The application became Docker-healthy with zero further restarts;
  `/v1/ready` reported DB1, retrieval, and modules OK. Database and embedder
  containers remained running throughout.
- The installed module and SPA hashes matched the tested artifacts. Runtime-web
  confirmed loading the updated SPA. Browser error rendering was tested locally;
  no operator browser session was used on production.

The raw post-recreation results and artifact hashes are in
`provider-probes-103-20260917.json`. MiniMax's configured lowercase model ID is
not an exact match in discovery (`model_available:false`), but real inference
succeeds (`execution_ok:true`), which is the GUI's availability signal.

## Deployment and recovery

The build is based on `origin/testing` at `4831f5bd03` and changes the Go
providers/egress modules plus the SPA. Source fix: `5378d49359`.
The local image `aimee:0.4.4-probe-fix-20260917` derives from the installed
`ghcr.io/rakuensoftware/aimee:0.4.4`, replacing only:

- `/usr/local/libexec/aimee-modules/aimee-module-providers`
- `/usr/local/libexec/aimee-modules/aimee-module-egress`
- `/usr/local/share/aimee-runtime-web/index.html`

The host's `/opt/aimee/compose.server-managed.yaml` includes the new
`deploy/container/provider-probes.override.yaml`, selecting that image.
Compose's normalized configuration was compared before deployment to confirm
only the application image changed. The application was recreated with
`--no-deps --no-build --pull never`.

Restart exposed an existing operational limit: the mandatory startup audit
verification takes longer than the launcher's 120-second deadline on this
39.8-million-row ledger (13.6 GB logical size, about 3.6 GB allocated). The first
start was terminated by that deadline. The override now sets
`AIMEE_WFE_SOCKET_WAIT_TENTHS: "12000"` and a 20-minute healthcheck start period.
Verification then completed and the API became ready. No audit data or
verification rules were changed.

Original module backups are inside the persistent application volume at
`/var/lib/aimee/repairs/provider-probes-20260917/`. The original Compose include
file is backed up under `/opt/aimee/repairs/provider-probes-20260917/` on CT103.
To roll back the image, change only the override's `image` back to
`ghcr.io/rakuensoftware/aimee:0.4.4` and recreate the application with the same
Compose command; retain the longer startup allowance for this ledger.
The repair image is local to CT103, not a published release.
