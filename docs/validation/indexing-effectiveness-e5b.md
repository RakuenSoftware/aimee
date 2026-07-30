# E5b deployment readiness validation

- Base: `origin/testing` at `ada6d21d9ac4461b9a144acdc1d8b5c1b43718c3`
- Environment: Linux x86_64, 2026-07-30 UTC
- Scope: retrieval readiness, restart/liveness policy, diagnostics, recovery runbook

Commands:

```sh
make build/obj/tests/unit-test-server-ready
build/obj/tests/unit-test-server-ready
git diff --check
```

The focused unit test passes. It proves unsampled/stale snapshots fail closed,
liveness dependencies are named, retrieval failure drains traffic, and breaker retry
and last-success diagnostics remain visible. The deployment audit confirms all shipped
split-stack services use `restart: unless-stopped`; server Docker healthchecks remain
on `/v1/health`, while the runbook assigns `/v1/ready` to traffic admission.

## Frozen-diff review

- Runs `oprun_g6a6b1ed93980fd5d_1785407513_2` and
  `oprun_g6a6b1ed93980fd5d_1785407524_3` received empty payloads because the
  replacement host lacked the expected JSON utility. They are invalid transport
  attempts and are not review evidence.
- Run `oprun_g6a6b1ed93980fd5d_1785407534_4` converged with all three participants
  and requested changes. It found that the untracked runbook was absent from the
  frozen artifact, collapsed retrieval diagnostics did not identify the failed
  boundary, dependency text was not JSON-escaped, and retrieval-only failure lacked
  a direct renderer regression. New files are now included in the frozen artifact;
  the exact failed boundary is reported; diagnostic strings are escaped; and both
  retrieval-only failure and hostile diagnostic text have regressions.
- Run `oprun_g6a6b1ed93980fd5d_1785407898_5` is invalid review evidence. Its artifact
  was generated against the moving `origin/testing` ref after unrelated PR #2174
  advanced that ref, so it mixed the E5b patch with a reverse diff of upstream Vault
  changes. The panel correctly rejected that out-of-scope mixed artifact. Subsequent
  review artifacts are generated against immutable branch base `HEAD`.
- Run `oprun_g6a6b1ed93980fd5d_1785408285_6` reviewed the correct seven-file
  artifact and found one remaining JSON boundary: a fresh render with missing
  diagnostics could interpolate uninitialized escape buffers. The escaper now
  initializes every nonzero-capacity destination before accepting a null source,
  and a fresh/null-diagnostics regression proves deterministic fail-closed JSON.
- Run `oprun_g6a6b1ed93980fd5d_1785408651_7` approved and converged with all three
  participants, no degradation, and no findings. Frozen artifact SHA-256:
  `0f0e27dbb596f737174e815004baf9a71b904127185a7af05809c747bdceb8e0`.
