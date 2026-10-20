# Optional native subprocess containment primitives

`server_native_containment` owns an optional Linux cgroup-v2 boundary. Absence of
an explicit delegated root leaves existing native launch behavior available;
those launches do not acquire a whole-tree crash-recovery proof. A configured
boundary that cannot be established must refuse that qualified launch. A PID,
process group, unlocked cache lease or model completion never substitutes for it.
These primitives do not themselves call native-cache recovery or reconcile a
canonical execution.

The host exclusively creates a random leaf beneath a user-owned delegated root
on real cgroup2fs. The bounded typed manifest binds machine ID, boot ID, host mount
namespace, root and leaf device/inode identities, random leaf name and the fixed
pre-exec protocol. Every reopen and empty/kill operation rechecks the exact leaf
identity. Replacement, removed, foreign-boot and foreign-namespace descriptors
refuse. A leaf is removed only after its owner has been reconciled and it is empty.

The native Codex/Claude descriptor spawners accept an optional two-sided guard.
Before SDK exec, its child closes unrelated descriptors and waits on a pipe; the
parent moves that exact forked child into the retained leaf. A second handshake
admits exec only after the child enters private user, mount, PID and cgroup
namespaces, mounts private `/proc`, masks `/sys/fs/cgroup` with read-only empty
tmpfs, drops effective/permitted/inheritable/bounding capabilities, locks root
capability regain and sets `no_new_privs`. The host requires a single canonical
cgroup2 mount and no alternate whole-proc mount before this path is available.
The cgroup namespace is rooted at the leaf: even a nested user namespace cannot
mount ancestor migration controls. The approved cwd is reopened through the new mount namespace, checked against
its original device/inode and selected there; its old directory descriptor is
closed before SDK exec even when not marked CLOEXEC. No inherited host descriptor
can reveal the old proc/cgroup mount. The v3 protocol additionally installs Landlock ABI 4 filesystem/network rules and
an architecture-checked seccomp filter before releasing the exec barrier.
Only TCP stream sockets can be created; connect is restricted to the exact owned
runtime proxy port list and bind is denied. Unix sockets, datagrams, other socket
families and io_uring setup are denied. Local Unix stream socketpairs remain
available, but cannot connect, bind or listen on a host control endpoint.
Landlock grants ports, not destination addresses: another address using the same
allowed port remains reachable. This is an explicitly bounded runtime-port
profile, not a general hostile-network isolation guarantee.

Filesystem writes are denied except inside the freshly materialized private SDK
HOME and its resolved canonical sessions/projects state subtree. The approved
cwd must be that HOME's workspace. The helper validates the native HOME layout,
private ownership/modes and exact canonical cache target. Binding/control parents
receive no write, remove, rename or hardlink grants. Thus the SDK cannot replace
the held owner lock, marker, binding directory or their ancestors while retaining
its normal private config/cache writes. Device and socket creation are not
granted. Read access continues under the existing host filesystem contract.

A trusted direct-child wrapper waits for the SDK's private namespace PID1;
namespace PID1 termination also kills its namespace descendants. A daemon crash
may still leave this entire boundary active. Recovery must reopen the durable
exact manifest and observe real `cgroup.events` `populated 0`, optionally after a
bounded `cgroup.kill`; direct-child death alone remains insufficient. EOF before
parent admission prevents SDK execution. This is not a guarantee against a
host-privileged attacker, kernel compromise, or a host owner intentionally moving
processes; the SDK receives no such host control authority.

Before selecting this optional path, `server_native_containment_probe` runs the
same namespace/barrier/privilege-drop, seccomp and Landlock sequence with a
harmless disposable child and a disposable surrogate HOME/cache layout. It does
not inspect or materialize the actual SDK HOME/cache.
Probe failure can preserve the ordinary path before any SDK HOME/cache access or
canonical reservation. A later real-owner setup failure cannot fall back after
SDK exec; the successful probe does not establish another boundary's quiescence.

The kernel-backed fixture requires `AIMEE_TEST_NATIVE_CGROUP_ROOT` pointing to an
actual writable delegated root. Missing configuration reports one explicit skip;
a configured unavailable or rejected boundary fails. It exercises real namespace
admission, unavailable ancestor controls, owner SIGKILL while a `setsid` writer
continues, exact-boundary kill/empty, stale identity refusal and owner death
before admission with no SDK execution. It also checks old-cwd descriptor and
proc traversal, actual allowed/denied TCP listeners, host Unix IPC refusal,
stream-socketpair non-reconnection, UDP/io_uring refusal, permitted state writes,
and refused lock unlink/replacement/truncation/rename/hardlink/alias writes while
a competing flock remains excluded. Filesystem mocks do not qualify this
boundary. Existing native fixtures without a guard continue to cover normal
Codex/Claude paths.

Persistent canonical reservation, atomic old-source cancellation/writer fencing,
cache invalidation/epoch change and exact claim reconciliation are separately
owned operations. They must be committed before releasing native cache custody;
this helper grants no permission to clear a cache marker or infer task success.

## Native execution integration and qualification status

`server_native_execution` uses these primitives with durable session reservations.
It reserves ownership before materializing SDK state, binds the pending cache
claim after configuration attestation, and holds the runtime port until the
whole boundary is empty. Failed cleanup parks bounded custody for retry; it does
not mark the source successful or release cache authority. Session-wide discovery
runs before primary history/model preparation so abandoned owners from a different
SDK or selected model cannot be silently skipped. Reconciliation checks the exact
stored descriptor and lease before cancelling/fencing the old source and changing
the shared cache epoch. Old physical SDK state is retained.

The current integration has actual-kernel and sanitizer fixture coverage. Eight
pinned SDK wire/credential configurations passed first/resumed-turn custody checks.
A focused actual-daemon crash journey now passes for both pinned SDKs: surviving
frozen descendants are quiesced, the old turn is cancelled/fenced exactly once,
cache epochs rotate, and fresh turns preserve the selected model and old files.
Interrupted host questions produce source-bound unknown receipts. A new mutation
requires its own operator permission; denial suppresses later attempts in that
turn, and replay neither reopens controls nor repeats effects. The focused journey
does not replace the full eight-profile integration or fresh CI.

These checks qualify the configured Linux boundary, not live vendor subscriptions
or non-Linux native server execution. macOS and Windows currently run the thin
client against the server. Production has not been promoted.
