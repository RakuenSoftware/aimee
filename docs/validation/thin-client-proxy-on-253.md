# Thin-client proxy validation on .253

Date: 2026-09-06. Host: `root@192.168.1.253`, Proxmox 9.2.4.

## Live failure and repair

The live `.103` model roster named `gpt-6-astra-high`. A non-streaming
Responses request exposed the provider's HTTP 400 detail: that model is not
supported for the ChatGPT account. The streaming route discarded this detail
and emitted only `upstream model request failed`.

The trusted Proxmox connection identified `.103` as CT 103 and independently
verified its changed SSH fingerprint. Through that connection, only the
`codex` entry's `model` field was changed to `gpt-6-astra`. All other bytes,
credentials, limits, roles, and file ownership were preserved. An atomic
replacement was used; Aimee reloaded the roster without a service restart.
The recovery copy remains inside the production server container at
`/var/lib/aimee/models.json.before-proxy-model-fix-20260906T124800Z`.

Verification after repair:

- Live mTLS `/v1/responses`: HTTP 200, `response.completed`, `PROXY_OK`, no
  `response.failed`.
- Real Codex 0.153.4 via the newly built `aimee launch --gateway -- codex`:
  `PROXY_OK`, exit 0.
- The server-side error-reporting source change compiles, and its normal and
  ASan/UBSan error-shaping tests pass. This source change has not been deployed
  over the installed v0.4.1 server; the checkout is an older version baseline.

## Fresh-guest acceptance

Run `bash scripts/validate-proxy-on-proxmox.sh`. Host, storage, guest IDs,
template, image, and SSH public key can be overridden with environment variables.
The runner refuses occupied IDs and checks each created guest's unique name
before deleting it. Cleanup runs on success, failure, and handled signals.

| Guest | Environment | Result |
| --- | --- | --- |
| CT 9201 | Fresh unprivileged Debian 13.1 CT; 2 CPU, 2 GiB RAM, 8 GiB disk | 26/26 proxy tests; error-shaping and 9 profile-parser tests passed |
| VM 9202 | Fresh Debian 13.6 cloud-image VM; 2 CPU, 2 GiB RAM, 8 GiB disk | 26/26 proxy tests; error-shaping and 9 profile-parser tests passed |

Both installed Codex 0.153.4 and the identical thin-client artifact:
`c4fedd36defae083c5b5cfd4623cd415e966764f81afb32b21d5f2da2a94c2bd` (SHA-256).
Final run identifier: `FZX9Ptsc` (2026-09-06, approximately 13:20–13:24 UTC).
Each suite used a fresh temporary Aimee/Codex home and deterministic HTTP/mTLS
peers. Production credentials were not copied into either guest. Coverage
includes real Codex Responses parsing, mTLS authentication, pin/identity
refusal, credential isolation, request framing, route restrictions, streaming,
concurrency, cancellation, process exit, and listener shutdown. The installed
plugin regression proves the hook runs in the positive control and is disabled
by proxy launch, while a separate user hook still runs. It also checks that the
base config is unchanged and the temporary profile is removed. Additional
tests cover profile-creation failure, explicit profiles, and profile cleanup
after child failure and handled termination. The same 26 tests passed locally
with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

The first CT attempt stopped before tests because npm's global executable
directory was absent from the guest PATH. That guest was deleted by the failure
cleanup. The harness now explicitly adds npm's global bin directory; the passing
CT and VM runs above were provisioned fresh afterward.

After the passing run, both guest configurations were absent, `pvesm list optane
--vmid 9201` and `--vmid 9202` returned no volumes, and the run's temporary host
payload was absent. The original running guests remained running. Only the
disposable test guests and their data were deleted; they are not recoverable.

## Scope and delivery status

The fresh-guest suite is a thin-client transport/launcher suite, not a complete
fresh deployment of every Aimee server/KB module. The live provider check above
used the existing `.103` deployment.

The previously observed Codex 0.153.4 hook-discovery limitation is addressed by
a launcher-owned temporary profile. A live check with a file profile returned
`PROXY_OK` without the installed Aimee SessionStart hook. The automated final
artifact test above covers creation and removal of that profile. Explicit
user-selected Codex profiles are preserved; those profiles must disable
`plugins."aimee@local"` themselves, as the launcher warns. No global plugin or
hook-trust settings were changed.

Git publication was initially blocked. Registered Aimee Git calls waited for the workspace
runner and rejected the unavailable session checkout. During investigation,
the shared `.103` service intermittently became unhealthy: its log recorded
module-call `deadline_exceeded` and mTLS re-check `ERROR` (not confirmed
revocation), returning HTTP 403 for runner and other requests. The temporary
runner and pending CLI attempts were stopped. Credentials were not rotated and
the shared service was not restarted. The operator assigned `.103` repair to a
different session and explicitly authorized regular Git CLI publication of this
change, without further changes to the shared service.
