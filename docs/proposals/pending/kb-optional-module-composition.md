# KB-optional deployment through module composition

Status: implementation in progress for the requested 0.4.2 release scope. The existing
memory-routing repairs are separately tested; they do not establish this deployment yet.

Memory is already a process module available in both Server and KB compositions. Extend
that implementation. Database ownership and authenticated access grants determine which
store a module can address. Do not create separate Server and KB memory implementations.

Embedding and synthesis services remain coupled to the composition that uses them. Either
composition can provision local model containers or configure external targets. Their
credentials must come from the owning composition's authorized identity services, without
requiring a KB to bootstrap a personal deployment. Choosing a remote KB must not hide or
replace the user's local embedding and synthesis configuration.

The local composition must support first boot, personal-memory storage, vector persistence,
recall and optional synthesis with no KB container, endpoint, database or identity provisioner.
A configured KB adds access to shared knowledge. It never becomes fallback storage for
personal records or their derived data, and equal numeric IDs do not confer cross-store access.

Acceptance requires fresh-volume and upgrade tests, actual local model startup and inference,
module/grant denial tests, dependency outage and recovery, and canary checks proving that
personal data stays within the local composition. Shared deployments must retain their
existing success-path contracts and independent model configuration.
