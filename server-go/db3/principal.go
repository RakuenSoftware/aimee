package db3

import (
	"errors"
	"fmt"
)

// A DB3 vector provider is a dynamically provisioned process, exactly like a
// plugin module instance: an operator installs Qdrant or Milvus or another
// adapter, and it attaches to a bus. DB3Router keys providers by principal ref,
// but until now nothing constrained WHICH ref a provider could claim -- any
// non-zero value was accepted.
//
// That is the same missing allocation authority that let the plugin event range
// squat postgres's kind block. A provider claiming ref 7 or 28 would derive the
// event kinds of `memory` or `postgres`, and bus_host_serve_kind() binds one kind
// to exactly one serving slot, so whichever attaches second is denied -- possibly
// the core module, and the only trace is in its own log.
//
// The fix is the one the plugin path already uses: refs come from a band
// reserved in tests/baselines/modules/canonical-inventory.yaml, and event kinds
// are DERIVED from the ref by the canonical rule rather than chosen. The
// inventory gate refuses to assign any module a ref inside a reserved band, and
// refuses overlapping bands, so the two allocators cannot collide.
const (
	// ProviderRefFirst/ProviderRefLimit MUST match
	// `db3_provider_principal_ref_band` in the canonical inventory.
	ProviderRefFirst uint32 = 456
	ProviderRefLimit uint32 = 512

	// The canonical derivation, shared with every other module:
	//     kind = 4096 + principal_ref*256 + stage
	kindOrigin uint32 = 4096
	kindStride uint32 = 256
)

// ErrProviderRef reports a principal ref outside the reserved provider band.
var ErrProviderRef = errors.New("db3: invalid provider principal ref")

// ValidateProviderRef reports whether a ref may be used by a vector provider.
//
// Fail-closed: a provider that cannot prove it owns its identity must not be
// admitted, because the cost of getting this wrong is a CORE module being denied
// at attach rather than the provider itself failing.
func ValidateProviderRef(ref uint32) error {
	if ref < ProviderRefFirst || ref >= ProviderRefLimit {
		return fmt.Errorf("%w: %d is outside the reserved provider band [%d,%d)",
			ErrProviderRef, ref, ProviderRefFirst, ProviderRefLimit)
	}
	return nil
}

// ProviderKind returns the event kind a provider ref owns for a given stage.
//
// Derived, never supplied: two providers can only share a kind if they share a
// ref, which the band plus the provisioner's uniqueness check already prevent.
func ProviderKind(ref, stage uint32) (uint32, error) {
	if err := ValidateProviderRef(ref); err != nil {
		return 0, err
	}
	if stage >= kindStride {
		return 0, fmt.Errorf("%w: stage %d exceeds the %d-kind block a ref owns",
			ErrProviderRef, stage, kindStride)
	}
	return kindOrigin + ref*kindStride + stage, nil
}
