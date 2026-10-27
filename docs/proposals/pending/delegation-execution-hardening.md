# Proposal: separately review delegation execution and authority

- **State:** PENDING. Not part of the IR/browser migration or approved for rollout.
- **Origin:** execution features introduced during the IR migration, removed from
  that change at the repository owner's request on 2026-10-27.

## Problem and possible value

Canonical model messages do not by themselves define permission to start a
subprocess, use a workspace, obtain credentials, or recover a container after a
crash. Those concerns deserve an explicit design and scope review rather than
being introduced as incidental consequences of changing message representation.

Three additions may merit independent work:

1. Host-owned ACP execution and resource callbacks. The previous host execution
   path rejected unsupported native adapters; enabling ACP is a capability change.
2. Durable delegation authority binding a caller, workspace, credentials and
   source to a child execution. The removed implementation used authenticated,
   encrypted descriptors and propagated execution scope through workflow APIs.
3. Docker resource receipts, private helper access, durable leases and restart
   cleanup. These alter acquisition, mount and lifecycle behavior beyond IR.

## Review boundaries

Assess these as separate changes, with a concrete supported use case and the
smallest implementation for each. Preserve existing behavior unless the feature
is explicitly approved. Reuse existing identity, Vault, workspace and lifecycle
owners; avoid a parallel authority system or a new persisted contract merely to
carry information already available at dispatch.

Any future implementation must keep model messages in the existing canonical IR
and leave control, authority and workflow records as their own typed contracts.
Specify unsupported adapters, credential context, cancellation, crash recovery,
and compatibility before changing them. Tests should cover those boundaries
without creating another execution implementation solely for fixtures.

## Preserved implementation and acceptance

The pre-removal implementation is retained at commit
`9d257322da116facd5c9590d531dbd781d633916`, under local reference
`archive/ir-before-scope-strip-9d257322`. The removed implementation is preserved separately on branch
`proposal/deferred-execution-and-accounting`; its existence is not evidence of
approval or final qualification. Earlier browser qualification does not qualify a future feature
branch or its final source.

The IR/browser PR remains limited to canonical model-message conversion, shared
thinclient/browser repairs, and removal of duplicate or unused code. These
execution features and their exclusive tests belong outside that PR.
