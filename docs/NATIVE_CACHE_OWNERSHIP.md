# Native cache ownership

The canonical session log remains the history source. Native Codex and Claude
conversation files are a derived cache and cannot establish a completed turn,
tool effect, permission, or test grade.

The canonical Claude gateway launches the SDK with the fixed environment setting
`CLAUDE_CODE_ATTRIBUTION_HEADER=0`. The host prepares canonical system content,
so it follows Claude's [gateway compatibility protocol](https://code.claude.com/docs/en/llm-gateway-protocol#system-prompt-attribution-block)
by having the client omit its conversation-fingerprint block. The host does not
synthesize, move, or strip a billing prefix. This setting is part of the verified
launch-environment digest, checked again during SDK initialization; an absent,
changed, duplicate, or extra environment entry fails admission. Existing cache
profiles without this setting do not match the new launch profile. Git commit
and pull-request attribution requirements are unrelated and remain unchanged.

Claude's optional SDK `metadata.user_id` stays in the typed request metadata
through host request ownership. Only a unique object containing that optional
string/null field is accepted, with the provider's 512 Unicode-character bound.
The host retains its own metadata, rejects a conflicting host identifier and
copies SDK correlation afresh for each callback. This field establishes no
account, actor, session or effect authority. SDK system content, transcripts,
tool catalogs and raw request extensions do not replace their canonical owners.

Claude's scheduler cwd is a private `sdk-workspaces/{epoch}` directory beneath
that session and account/model binding's exclusive cache lease. Its pathname and
inode survive ordinary turns and A→B→A model selection within the same epoch;
the real operator project remains the tool runtime's separate authority. HOME,
configuration, capabilities and temporary files are still fresh per launch. The
host rechecks the private cwd path against its retained descriptor before launch
and initialization. Qualified containment binds the fresh HOME and exact SDK
state interior independently, leaving the stable scheduler cwd read-only.

The versioned scheduler-workspace policy participates in the Claude launch digest
and therefore the existing cache scope. Pre-fix handles whose project namespace
was keyed by a deleted temporary cwd are not adopted under this new contract.
Their files and canonical history remain intact; the host neither rewrites a
transcript nor substitutes a resume ID to make old handles appear valid.

Claude also retains one nonsecret 64-hex SDK device-correlation ID in the same
verified account/model cache lease. The host copies it into each fresh
`CLAUDE_CONFIG_DIR/.claude.json`; no credential, mutable SDK configuration or
operator account document is persisted with it. The SDK identity version and
actual ID participate in the launch digest. Pre-identity handles are therefore
not adopted, and replacing a valid ID changes cache compatibility. The original
canonical history and SDK files remain intact. The host validates the private
seed file before launch and initialization; unsafe lease/config state refuses
before model execution.

The host acquires an exclusive cache-directory lease before creating a native
HOME, linking SDK conversation files, writing launch configuration, or spawning
the SDK. Its key is the actual `browser-{sdk}/{session}/{binding}` directory;
context/catalog variants sharing that directory share the same lease. Account,
model and actor lineage remain part of the existing binding. The lock is retained
through child shutdown and the durable terminal cache completion/abort callback.
No SDK receives the lock descriptor or credentials from this mechanism.

A bounded, credential-free owner marker records the session, actor, source turn,
SDK, binding and a random owner nonce. It is synced before SDK access. Normal
completion/abort removes only that exact marker after the existing synchronous
direct-child/process-group shutdown and cache-owner transition. That existing
shutdown contract is not a new proof that escaped descendants are quiescent. A replaced marker cannot be removed by a
stale owner. The lock file remains in place to avoid inode-replacement races.
Existing SDK conversation files and unrelated committed checkpoints are retained.

A daemon crash leaves the marker unresolved. Restart refuses before native HOME
creation even when the old flock is available and the daemon PID is gone. The
real subprocess fixture demonstrates that an escaped `setsid` descendant can
continue writing cache data after the owner dies; neither flock release nor
individual process death is sufficient authority to clear the marker.

This increment establishes pre-spawn custody and fail-closed restart behavior;
it does **not** implement automatic stale-claim recovery. In particular:

- An absent legacy marker does not prove a legacy pending/finishing claim safe.
  Existing DB1 cache checks still refuse it; no legacy claim is cleared here.
- Recovery still needs positively attested whole-process-tree containment, a
  durable exact old claim/profile/source binding and a terminal source lifecycle.
  Running abandoned turns need a separate idempotent cancellation contract.
- A qualified optional cgroup-v2/supervisor boundary may supply that proof in a
  later increment. Cgroup availability is not a new launch prerequisite.
- Missing, corrupt or foreign markers never authorize recovery. There is no
  PID-based cleanup, cache-tree wipe, native-handle promotion, or automatic
  deletion of unresolved markers.

The lifecycle fixture covers ordinary Codex/Claude success and refusal paths,
lease transfer through the terminal callback, concurrent owner refusal and
cache-preserving canonical adoption. The lease fixture covers real owner
SIGKILL, a still-writing escaped descendant, stale/foreign marker refusal and
symlinked lock refusal. These tests do not qualify full automatic crash recovery.
