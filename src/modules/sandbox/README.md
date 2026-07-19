# modules/sandbox

The **delegate-image / package-proxy** sandbox: `sandbox_learned.c` (learned
toolchain image provisioning) and `sandbox_pkg_proxy[_core].c` (the package
download proxy used while building delegate images).

This is deliberately **separate** from the *process-isolation* sandbox in
`src/posix/sandbox.c` + `src/headers/sandbox.h` (`DEC_SANDBOX_H`,
`SANDBOX_MODE_OFF/WORKSPACE_ONLY/ALLOWLIST` — the Linux user/mount-namespace
jail). The two share the `sandbox_` name prefix but are unrelated concerns and
must not be re-merged: the namespace jail is platform (`posix/`) tier, this
module is the delegate-provisioning tier that consumes it.
