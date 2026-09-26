package memory

import (
	"encoding/json"
	"os"
	"time"
)

// This verifier attests only to current lifecycle/scope admission by the source
// owners while holding their v2 send guards. It is not a truth, usefulness,
// contamination or fusion-recovery label.
type healthReleaseCheck struct {
	GuardCheck string `json:"guard_check_id,omitempty"`
	Binding    string `json:"receipt_binding_sha256"`
	Check      string `json:"source_check_id"`
	Sources    string `json:"sources_digest"`
	ObservedAt string `json:"observed_at"`
	Verifier   string `json:"verifier"`
}

func healthReleaseValid(proof *healthReleaseCheck, b providerReceiptBinding, binding string, prepared time.Time) bool {
	if proof == nil || proof.Verifier != "source_owner_guard_v2" || proof.Binding != binding || !receiptDigestValid(binding) || proof.Check == "" || proof.Check != b.SourceCheckID || proof.Sources != b.SourcesDigest || !receiptDigestValid(proof.Sources) || b.SourceCoverage != "retained_versioned_inputs" {
		return false
	}
	observed, err := time.Parse(time.RFC3339Nano, proof.ObservedAt)
	return err == nil && !observed.After(prepared) && prepared.Sub(observed) <= 5*time.Second
}
func receiptMetadataWithRelease(metadata json.RawMessage, entry *sourceReleaseEntry, binding providerReceiptBinding, digest string, at time.Time) json.RawMessage {
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" || entry.guardedAdmission == "" || entry.guardedAdmission != binding.SourceCheckID {
		return metadata
	}
	proof := &healthReleaseCheck{Binding: digest, Check: entry.guardedAdmission, Sources: binding.SourcesDigest, ObservedAt: entry.guardedAdmissionAt.UTC().Format(time.RFC3339Nano), Verifier: "source_owner_guard_v2"}
	if !healthReleaseValid(proof, binding, digest, at) {
		return metadata
	}
	var fields map[string]json.RawMessage
	if json.Unmarshal(metadata, &fields) != nil || fields == nil {
		return metadata
	}
	fields["health_release"], _ = json.Marshal(proof)
	raw, err := json.Marshal(fields)
	if err != nil || len(raw) > 12000 {
		return metadata
	}
	return raw
}

// Preparation precedes socket acquisition. The actual v2 send guard has a new
// check ID; bind it to the exact pending receipt, then persist it in the existing
// dispatch-started observation. Never retrofit this later proof into assembly.
func (s *sourceReleaseState) healthGuardObserved(source *sourceReleaseEntry, check string) {
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" {
		return
	}
	receipt := s.receipts[source.healthAttempt]
	if receipt == nil || receipt.binding != source.binding || receipt.started != "" {
		return
	}
	var prepared providerReceiptEvent
	if json.Unmarshal([]byte(receipt.prepared), &prepared) != nil || prepared.Binding == nil || prepared.Binding.SourcesDigest != source.digest {
		return
	}
	receipt.healthRelease = &healthReleaseCheck{Binding: receipt.digest, Check: prepared.Binding.SourceCheckID, GuardCheck: check, Sources: source.digest, ObservedAt: source.guardedAdmissionAt.UTC().Format(time.RFC3339Nano), Verifier: "source_owner_guard_v2"}
}
func healthDispatchReleaseValid(dispatch, prepared *providerReceiptEvent) bool {
	if dispatch == nil || prepared == nil || prepared.Binding == nil || dispatch.Stage != "dispatch_started" || dispatch.AttemptID != prepared.AttemptID || dispatch.BindingDigest != prepared.BindingDigest || dispatch.HealthRelease == nil || !releaseTokenValid(dispatch.HealthRelease.GuardCheck) {
		return false
	}
	start, err := time.Parse(time.RFC3339Nano, dispatch.At)
	if err != nil || !healthReleaseValid(dispatch.HealthRelease, *prepared.Binding, prepared.BindingDigest, start) {
		return false
	}
	before, err := time.Parse(time.RFC3339Nano, prepared.At)
	observed, e := time.Parse(time.RFC3339Nano, dispatch.HealthRelease.ObservedAt)
	return err == nil && e == nil && !observed.Before(before)
}
