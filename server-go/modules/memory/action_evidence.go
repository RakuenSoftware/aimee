package memory

import (
	"encoding/json"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// actionEvidence is an observation for execution policy, not an action permit.
// It commits the exact retained source set and provider receipt, and is emitted
// only after a guarded revalidation. The host holds the corresponding owner
// guards through its durable dispatch decision and releases them afterwards.
func (s *sourceReleaseState) actionEvidence(entry *sourceReleaseEntry) ([]byte, bus.ModuleStatus) {
	unavailable := func() ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("unavailable", "current guarded action evidence unavailable"))
	}
	now := time.Now().UTC()
	if entry.guardedAdmission == "" || entry.guardedAdmissionAt.IsZero() || now.Sub(entry.guardedAdmissionAt) > time.Second || entry.guardedAdmissionAt.After(now) {
		return unavailable()
	}
	receipt := s.receipts[entry.healthAttempt]
	if receipt == nil || receipt.binding != entry.binding || receipt.observation == "" || receipt.started == "" {
		return unavailable()
	}
	var prepared, observed providerReceiptEvent
	if json.Unmarshal([]byte(receipt.prepared), &prepared) != nil || prepared.Binding == nil || json.Unmarshal([]byte(receipt.observation), &observed) != nil || observed.Stage != "acknowledged" || observed.HTTPStatus < 200 || observed.HTTPStatus >= 300 {
		return unavailable()
	}
	if prepared.BindingDigest != receipt.digest || observed.BindingDigest != receipt.digest || releaseDigest(prepared.Binding) != receipt.digest || releaseDigest(json.RawMessage(prepared.Binding.Sources)) != releaseDigest(json.RawMessage(entry.sources)) {
		return unavailable()
	}
	return commandResult(map[string]any{"status": "ok", "evidence_sha256": releaseDigest(json.RawMessage(entry.sources)), "context_receipt": receipt.digest, "revocation_generation": entry.digest, "memory_guard": entry.guardedAdmission, "checked_at": now, "expires_at": now.Add(time.Second), "memory_required": true})
}
