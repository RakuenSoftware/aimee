package memory

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"strconv"
	"time"
)

// Horizon evaluation is pure. Its inputs are owner snapshots;
// this type is not a public write or an authorization mechanism. Integration
// must acquire anchors from canonical creation/confirmed-update events, never
// use last-access/use-count timestamps as confirmation.
type horizonAnchor struct {
	At      string              `json:"at"`
	EventID string              `json:"event_id"`
	Version MemoryRecordVersion `json:"version"`
}
type horizonRecord struct {
	Version   MemoryRecordVersion `json:"version"`
	Kind      string              `json:"kind"`
	Domain    string              `json:"domain"`
	Created   horizonAnchor       `json:"created"`
	Confirmed *horizonAnchor      `json:"confirmed,omitempty"`
	Override  *horizonRule        `json:"override,omitempty"`
	// Only a verified owner admission may populate Override. A model-proposed
	// rule is a proposal, not part of this admitted record snapshot.
	BaseEligible bool `json:"base_eligible"`
}
type horizonRule struct {
	ID              string `json:"id"`
	DurationSeconds int64  `json:"duration_seconds"`
	Anchor          string `json:"anchor"`
	Deadline        string `json:"deadline,omitempty"`
}
type horizonPolicy struct {
	SchemaVersion int    `json:"schema_version"`
	Revision      string `json:"revision"`
	// Initial policies are explicitly classified transient kinds only. There
	// is deliberately no wildcard default and no learned automatic override.
	TransientKinds map[string]bool        `json:"transient_kinds"`
	Safety         map[string]horizonRule `json:"safety"`
	Domains        map[string]horizonRule `json:"domains"`
	Kinds          map[string]horizonRule `json:"kinds"`
	UnknownRule    string                 `json:"unknown_rule"`
}
type horizonDecision struct {
	PolicyRevision string              `json:"policy_revision"`
	PolicyDigest   string              `json:"policy_digest"`
	RecordVersion  MemoryRecordVersion `json:"record_version"`
	Mode           string              `json:"mode"`
	Purpose        string              `json:"purpose"`
	EvaluatedAt    string              `json:"evaluated_at"`
	Status         string              `json:"status"`
	Reason         string              `json:"reason"`
	RuleID         string              `json:"rule_id,omitempty"`
	RuleSource     string              `json:"rule_source,omitempty"`
	AnchorEvent    string              `json:"anchor_event,omitempty"`
	Deadline       string              `json:"deadline,omitempty"`
	Elapsed        bool                `json:"elapsed"`
	WouldExclude   bool                `json:"would_exclude"`
}

func validHorizonRule(r horizonRule) bool {
	if r.Deadline != "" {
		at, err := time.Parse(time.RFC3339Nano, r.Deadline)
		if err != nil || at.Year() < 1 || at.Year() > 9999 {
			return false
		}
	}
	return r.ID != "" && len(r.ID) <= 128 && r.DurationSeconds >= 0 && r.DurationSeconds <= 10*366*24*60*60 && (r.Anchor == "created" || r.Anchor == "confirmed")
}
func (p horizonPolicy) valid() bool {
	if p.SchemaVersion != 1 || p.Revision == "" || len(p.Revision) > 128 || (p.UnknownRule != "exclude" && p.UnknownRule != "allow") || len(p.TransientKinds) > 64 || len(p.Safety) > 64 || len(p.Domains) > 64 || len(p.Kinds) > 64 {
		return false
	}
	for _, rules := range []map[string]horizonRule{p.Safety, p.Domains, p.Kinds} {
		for key, rule := range rules {
			if key == "" || key == "*" || len(key) > 128 || !validHorizonRule(rule) {
				return false
			}
		}
	}
	for kind := range p.TransientKinds {
		if kind == "" || kind == "*" || len(kind) > 128 {
			return false
		}
	}
	return true
}

// Evaluation never reads a clock, mutates a record, renews an anchor, or grants
// historical access. Base eligibility represents separately enforced scope,
// lifecycle and erasure admission; no purpose may override that decision.
func evaluateUtilityHorizon(record horizonRecord, p horizonPolicy, purpose string, now time.Time) horizonDecision {
	raw, _ := json.Marshal(p)
	d := horizonDecision{PolicyRevision: p.Revision, PolicyDigest: fmt.Sprintf("sha256:%x", sha256.Sum256(raw)), RecordVersion: record.Version, Mode: "shadow", Purpose: purpose, EvaluatedAt: now.UTC().Format(time.RFC3339Nano)}
	unknown := func(reason string) horizonDecision {
		d.Status = "unknown"
		d.Reason = reason
		d.WouldExclude = purpose == "current" && p.UnknownRule != "allow"
		return d
	}
	if !record.BaseEligible {
		d.Status = "blocked"
		d.Reason = "base_eligibility_refused"
		d.WouldExclude = true
		return d
	}
	if !p.valid() || now.IsZero() || now.Year() < 1 || now.Year() > 9999 {
		d.Status = "unknown"
		d.Reason = "invalid_policy_or_clock"
		d.WouldExclude = true
		return d
	}
	switch purpose {
	case "current", "historical", "diagnostic":
	default:
		d = unknown("unsupported_purpose")
		d.WouldExclude = true
		return d
	}
	if !p.TransientKinds[record.Kind] {
		d.Status = "not_applicable"
		d.Reason = "kind_not_explicitly_transient"
		return d
	}
	var rule horizonRule
	var found bool
	if rule, found = p.Safety[record.Kind]; found {
		d.RuleSource = "system_safety"
	} else if record.Override != nil {
		rule = *record.Override
		found = true
		d.RuleSource = "record_override"
	} else if rule, found = p.Domains[record.Domain]; found {
		d.RuleSource = "domain"
	} else if rule, found = p.Kinds[record.Kind]; found {
		d.RuleSource = "kind_default"
	}
	if !found {
		d.Status = "not_applicable"
		d.Reason = "no_configured_duration"
		return d
	}
	d.RuleID = rule.ID
	if !validHorizonRule(rule) {
		return unknown("invalid_admitted_override")
	}
	anchor := record.Created
	if rule.Anchor == "confirmed" {
		if record.Confirmed == nil {
			return unknown("confirmation_missing")
		}
		anchor = *record.Confirmed
	}
	// Anchor identity must match the exact evaluated version. A confirmation
	// from another owner, another record, or an old revision cannot renew it.
	id, err := strconv.ParseInt(record.Version.RecordID, 10, 64)
	if err != nil || !record.Version.validFor(id) || anchor.Version != record.Version || anchor.EventID == "" || len(anchor.EventID) > 128 {
		return unknown("anchor_version_unavailable")
	}
	at, err := time.Parse(time.RFC3339Nano, anchor.At)
	if err != nil || at.Year() < 1 || at.After(now) {
		return unknown("anchor_time_unavailable_or_future")
	}
	deadline := at.Add(time.Duration(rule.DurationSeconds) * time.Second)
	if deadline.Year() > 9999 {
		return unknown("deadline_overflow")
	}
	if rule.Deadline != "" {
		explicit, _ := time.Parse(time.RFC3339Nano, rule.Deadline)
		if explicit.Before(deadline) {
			deadline = explicit
		}
	}
	d.AnchorEvent = anchor.EventID
	d.Deadline = deadline.UTC().Format(time.RFC3339Nano)
	d.Elapsed = !now.Before(deadline)
	d.Status = "eligible"
	d.Reason = "utility_horizon_unelapsed"
	if d.Elapsed {
		d.Reason = "utility_horizon_elapsed"
		if purpose == "current" {
			d.Status = "would_exclude"
			d.WouldExclude = true
		} else {
			d.Reason = "utility_horizon_elapsed_inspection_only"
		}
	}
	return d
}
