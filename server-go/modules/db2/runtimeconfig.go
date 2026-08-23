package db2

import "sync"

// The configuration values a ported operation reads.
//
// The C has an equivalent: db2_runtime_config_t, an immutable startup snapshot
// the module runtime installs before any worker thread or request dispatch, so
// nothing reads a half-written value and nothing re-reads configuration
// mid-request. This is the same idea with the same lifetime.
//
// It carries only what a ported operation actually reads rather than mirroring
// the C struct field for field, because a field nothing reads is a claim that
// the Go module honours a setting when it does not. It grows one field at a
// time, as the operation that needs it is ported.
//
// Nothing installs one yet: the module is deliberately absent from the registry
// until the ownership cutover, and the host that installs the C snapshot will
// have to install this one at the same point. Until it does, every value here
// is the zero value and every reader falls back to the same default the C falls
// back to -- which is correct for a default-configured deployment and wrong,
// silently, for a configured one. That is the cutover's problem to close, and
// it is written down here so it is not discovered instead.
type RuntimeConfig struct {
	// KBPurgeFenceTTLSeconds bounds how long a purge fence stays believable
	// without a heartbeat. Zero means unset, not "expire immediately".
	KBPurgeFenceTTLSeconds int
}

// kbPurgeFenceTTLDefault is the C's KBRS_FENCE_TTL_DFLT.
const kbPurgeFenceTTLDefault = 900

var (
	runtimeConfigMu sync.RWMutex
	runtimeConfig   RuntimeConfig
)

// InstallRuntimeConfig replaces the snapshot every ported operation reads.
//
// Guarded rather than a plain assignment because the C installs its snapshot
// before dispatch begins and this one has no such guarantee to lean on -- a
// host that installs late would otherwise race every in-flight request.
func InstallRuntimeConfig(snapshot RuntimeConfig) {
	runtimeConfigMu.Lock()
	defer runtimeConfigMu.Unlock()
	runtimeConfig = snapshot
}

// kbPurgeFenceTTLSeconds answers the configured TTL, or the default when it is
// unset. Greater than zero, exactly as the C tests it: a zero or negative
// setting is treated as absent rather than as an instant expiry, so a
// misconfiguration cannot make every fence read as dead.
func kbPurgeFenceTTLSeconds() int {
	runtimeConfigMu.RLock()
	defer runtimeConfigMu.RUnlock()
	if runtimeConfig.KBPurgeFenceTTLSeconds > 0 {
		return runtimeConfig.KBPurgeFenceTTLSeconds
	}
	return kbPurgeFenceTTLDefault
}
