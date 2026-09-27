package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strconv"
)

// Versioned exposure is derived from the final retained source manifest, after
// the receipt reader has checked the host ledger. Preparation proves selection;
// only provider acknowledgement establishes delivery. Neither establishes use.
// This is the existing memory process contract consumed by learning via host
// transport; memory does not import the learning owner.
func procedureExposures(receipt *providerReceiptEvent, acknowledged bool) []map[string]any {
	out := []map[string]any{}
	if receipt == nil || receipt.Binding == nil {
		return out
	}
	var refs []typedProjectionRef
	if json.Unmarshal(receipt.Binding.Sources, &refs) != nil {
		return out
	}
	state := "retrieved"
	if acknowledged {
		state = "delivered"
	}
	for _, ref := range refs {
		if ref.Channel != "approved_procedures" || ref.Source == nil || ref.Source.Kind != "learning_procedure" || !validTypedSource(ref) {
			continue
		}
		v := ref.Source.Version
		out = append(out, map[string]any{"receipt_ref": receipt.BindingDigest, "request_id": receipt.Binding.RequestID, "attempt_id": receipt.AttemptID, "procedure": map[string]string{"owner_id": v.OwnerID, "procedure_id": v.RecordID, "revision": v.RecordRevision}, "state": state, "producer": "local_host_ledger"})
	}
	return out
}

// Historical retrieval artifacts remain availability evidence. Go owns memory
// exposure normalization; the C adapter only persists the returned identifiers.
func legacyExposurePlan(args commandArgs) ([]byte, bus.ModuleStatus) {
	turn := args.stringOr("turn_id", "")
	if turn == "" || len(turn) > 256 {
		return commandResult(commandError("invalid_argument", "turn_id required"))
	}
	var values []json.RawMessage
	if raw, ok := args["surfaced_ids"]; ok && string(raw) != "null" && json.Unmarshal(raw, &values) != nil {
		return commandResult(commandError("invalid_argument", "exact source identifiers required"))
	}
	if len(values) > 4096 {
		return commandResult(commandError("invalid_argument", "exposure limit exceeded"))
	}
	ids := []string{}
	for _, raw := range values {
		text := string(raw)
		var quoted string
		if len(text) > 0 && text[0] == '"' {
			if json.Unmarshal(raw, &quoted) != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			text = quoted
		}
		id, err := strconv.ParseInt(text, 10, 64)
		if err != nil || strconv.FormatInt(id, 10) != text || (len(raw) > 0 && raw[0] != '"' && (id > 9007199254740991 || id < -9007199254740991)) {
			return commandResult(commandError("invalid_argument", "exact source identifiers required"))
		}
		if id > 0 {
			ids = append(ids, text)
		}
	}
	role := args.stringOr("role", "Recall")
	if role == "" {
		role = "Recall"
	}
	return commandResult(map[string]any{"status": "ok", "turn_id": turn, "role": role, "query_fingerprint": args.stringOr("query_fingerprint", ""), "surfaced_ids": ids, "exposure_state": "retrieved_unverified"})
}

// Context gets a bounded projection of learning-owned experience, never an
// unbounded event history that can crowd out the procedure itself. Counts stay
// unchanged; omitted cohorts/references are explicitly partial coverage.
func compactProcedureExperience(raw string) (json.RawMessage, string, error) {
	var snapshot struct {
		Revision string                       `json:"revision"`
		State    string                       `json:"state"`
		Cohorts  []map[string]json.RawMessage `json:"cohorts"`
	}
	if err := json.Unmarshal([]byte(raw), &snapshot); err != nil {
		return nil, "", err
	}
	total := len(snapshot.Cohorts)
	partial := false
	if total > 4 {
		snapshot.Cohorts = snapshot.Cohorts[:4]
		partial = true
	}
	for _, cohort := range snapshot.Cohorts {
		for _, field := range []string{"counterexample_refs", "applicability_gaps", "event_refs", "cost_report_refs", "reported_trial_latencies"} {
			if fieldRaw, ok := cohort[field]; ok {
				var values []json.RawMessage
				if err := json.Unmarshal(fieldRaw, &values); err != nil {
					return nil, "", err
				}
				if len(values) > 4 {
					cohort[field], _ = json.Marshal(values[:4])
					partial = true
				}
			}
		}
	}
	coverage := "complete"
	if partial {
		coverage = "partial"
	}
	out, err := json.Marshal(map[string]any{"revision": snapshot.Revision, "state": snapshot.State, "cohorts": snapshot.Cohorts, "total_cohorts": total, "context_coverage": coverage})
	if err == nil && len(out) > 2048 {
		out, err = json.Marshal(map[string]any{"revision": snapshot.Revision, "state": "omitted_budget", "cohorts": []any{}, "total_cohorts": total, "context_coverage": "partial"})
	}
	return out, snapshot.Revision, err
}
