# Sandbox module

This module owns delegate image/toolchain provisioning and the mediated package proxy.

It does not own the Linux process-isolation jail. That boundary lives in the platform sandbox code
and controls user/mount namespaces, workspace roots, and allowlists. Provisioning consumes that
isolation contract; it must not duplicate or weaken it.

Changes need package-policy, cache-integrity, no-network, no-credential, degraded-isolation audit,
and cleanup tests.
