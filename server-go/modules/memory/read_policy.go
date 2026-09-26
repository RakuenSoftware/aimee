package memory

import (
	"bytes"
	"encoding/json"
	"time"
)

// MemoryReadPolicy selects temporal semantics, never an audience or permission.
// Omitting it preserves the legacy get/as_of inspection contract. Version one
// deliberately supports exact-ID reads only; other surfaces must not silently
// claim historical reconstruction by ignoring this object.
type MemoryReadPolicy struct {
	SchemaVersion int    `json:"schema_version"`
	Mode          string `json:"mode"`
	ValidAt       string `json:"valid_at,omitempty"`
	BelievedAt    string `json:"believed_at,omitempty"`
}

// MemoryReadResult describes the temporal policy actually applied. A missing or
// ineligible row produces the same empty result, including under RLS. This is
// not an evidence assessment, mutation version or release-generation receipt.
type MemoryReadResult struct {
	UtilityHorizon *horizonPolicyIdentity `json:"utility_horizon_policy,omitempty"`
	SchemaVersion  int                    `json:"schema_version"`
	PolicyVersion  string                 `json:"policy_version"`
	Mode           string                 `json:"mode"`
	ValidAt        string                 `json:"valid_at,omitempty"`
	ErrorCode      string                 `json:"error_code,omitempty"`
	Message        string                 `json:"message,omitempty"`
}

func (p MemoryReadPolicy) validate(placement Placement, operation, legacyAsOf string) *MemoryReadResult {
	r := &MemoryReadResult{SchemaVersion: 1, PolicyVersion: currentEligibilityPolicy, Mode: p.Mode, UtilityHorizon: currentHorizonIdentity()}
	refuse := func(code, message string) *MemoryReadResult {
		r.ErrorCode, r.Message = code, message
		return r
	}
	if p.SchemaVersion != 1 {
		return refuse("unsupported_version", "read_policy requires schema_version=1")
	}
	if operation != "get" {
		return refuse("unsupported_mode", "read_policy is supported only for exact-ID get")
	}
	if legacyAsOf != "" {
		return refuse("invalid_argument", "read_policy and legacy as_of cannot be combined")
	}
	if p.BelievedAt != "" {
		return refuse("unsupported_mode", "memory record belief-time reconstruction is unavailable")
	}
	switch p.Mode {
	case "current":
		if p.ValidAt != "" {
			return refuse("invalid_argument", "current mode uses the storage request clock; omit valid_at")
		}
	case "historical":
		if placement != PlacementKB {
			return refuse("unsupported_mode", "personal memory does not yet retain reconstructable historical versions")
		}
		when, err := parseMemoryTime(p.ValidAt)
		if err != nil || len(p.ValidAt) > 64 {
			return refuse("invalid_argument", "historical mode requires a valid absolute valid_at timestamp")
		}
		r.ValidAt = when.UTC().Format(time.RFC3339Nano)
	default:
		return refuse("unsupported_mode", "read_policy mode must be current or historical")
	}
	return r
}

func commandReadPolicy(args commandArgs) (*MemoryReadPolicy, bool) {
	raw, exists := args["read_policy"]
	if !exists {
		return nil, true
	}
	var policy *MemoryReadPolicy
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if d.Decode(&policy) != nil || policy == nil {
		return nil, false
	}
	return policy, true
}
