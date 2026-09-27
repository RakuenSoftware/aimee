package executionpolicy

import (
	"encoding/json"
	"errors"
)

// validate reconstructs reservations from immutable intent and audit records.
// A valid JSON document alone is insufficient: losing a counter must not mint
// new allowance after restart. This detects corruption, not a malicious DB owner.
func (j ActionJournal) validate() error {
	bad := func() error { return errors.New("inconsistent durable action journal") }
	if j.SchemaVersion != 1 || j.Principal == "" || j.Root == "" || j.Actions == nil {
		return bad()
	}
	if uint64(len(j.Audit)) != j.Sequence {
		return bad()
	}
	ids := map[string]string{}
	for key, r := range j.Actions {
		if key == "" || key != r.Intent.IdempotencyKey || r.Digest != actionDigest(r.Intent) || r.Intent.Principal != j.Principal || r.Intent.Root != j.Root || r.Intent.ID == "" {
			return bad()
		}
		if _, exists := ids[r.Intent.ID]; exists {
			return bad()
		}
		ids[r.Intent.ID] = key
	}
	states := map[string]string{}
	latest := map[string]ActionAuditEntry{}
	reserved := map[string]bool{}
	var calls, work uint64
	sensitive, elevated := false, false
	for n, event := range j.Audit {
		key, exists := ids[event.Action]
		if !exists || event.Sequence != uint64(n+1) || event.At.IsZero() {
			return bad()
		}
		r := j.Actions[key]
		prior := states[event.Action]
		valid := false
		switch event.State {
		case "prepared":
			valid = prior == ""
		case "admitted":
			valid = prior == "prepared"
			units, err := actionUnits(r.Intent.WorkUnits)
			if err != nil || calls == ^uint64(0) || units > ^uint64(0)-work {
				return bad()
			}
			reserved[event.Action] = true
			calls++
			work += units
			sensitive = sensitive || r.Intent.Class == "sensitive_read"
			elevated = elevated || r.Intent.Class == "privilege_elevation"
		case "dispatching":
			valid = prior == "admitted"
		case "acknowledged", "effect_confirmed", "outcome_unknown", "failed_before_effect":
			valid = prior == "dispatching" || prior == "outcome_unknown" || (prior == "acknowledged" && event.State != "failed_before_effect") || (event.State == "failed_before_effect" && (prior == "prepared" || prior == "admitted"))
		}
		if !valid {
			return bad()
		}
		states[event.Action] = event.State
		latest[event.Action] = event
	}
	if calls != j.ReservedCalls || work != j.ReservedWork || sensitive != j.SensitiveRead || elevated != j.PrivilegeElevated {
		return bad()
	}
	for _, r := range j.Actions {
		e, ok := latest[r.Intent.ID]
		if !ok || r.State != e.State || r.Sequence != e.Sequence || !r.At.Equal(e.At) {
			return bad()
		}
		if reserved[r.Intent.ID] && r.Freshness == nil {
			return bad()
		}
		if r.Outcome != nil && (r.Outcome.State != r.State || r.Outcome.Destination != r.Intent.Destination || r.Outcome.PayloadDigest != r.Intent.PayloadDigest || r.Outcome.EvidenceRef != e.EvidenceRef) {
			return bad()
		}
	}
	return nil
}

// InspectAction exposes a receipt only after the durable owner authenticated the
// principal/session lineage. It never infers completion from model text.
func InspectAction(principal, root string, state []byte, id string) (ActionReceipt, error) {
	var journal ActionJournal
	if len(state) == 0 || len(state) > 4<<20 || json.Unmarshal(state, &journal) != nil || journal.Principal != principal || journal.Root != root || journal.validate() != nil {
		return ActionReceipt{}, errors.New("action journal unavailable")
	}
	for _, r := range journal.Actions {
		if r.Intent.ID == id {
			return r, nil
		}
	}
	return ActionReceipt{}, errors.New("action receipt unavailable")
}
