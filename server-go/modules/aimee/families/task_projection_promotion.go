package families

import (
	"encoding/json"
	"strconv"
)

// Freeze the exact revision while the host submits its draft to the existing
// private review owner. Failed/ambiguous submission retains this proof for an
// identical retry; a different edit cannot silently replace the pending draft.
func taskProjectionPromotion(state *taskProjectionState, r taskProjectionRequest, prepared taskPreparedView, save func() error) (uint32, []string, error) {
	if state.Binding.Store != "user" {
		return taskProjectionReply("unavailable", "shared_promotion_adapter_unavailable", nil)
	}
	if r.ClaimIndex == nil || *r.ClaimIndex < 0 || *r.ClaimIndex >= len(state.Items) {
		return taskProjectionReply("error", "selected_claim_required", nil)
	}
	item := state.Items[*r.ClaimIndex]
	if item.Kind != "hypothesis" && item.Kind != "working_decision" {
		return taskProjectionReply("error", "only_claims_can_be_proposed", nil)
	}
	n, e := strconv.ParseInt(r.TargetID, 10, 64)
	if e != nil || n < 1 || strconv.FormatInt(n, 10) != r.TargetID {
		return taskProjectionReply("error", "existing_canonical_target_required", nil)
	}
	var refs []struct {
		ID     string `json:"stable_id"`
		Source struct {
			Kind    string          `json:"record_kind"`
			Version json.RawMessage `json:"version"`
		} `json:"source_version"`
	}
	if json.Unmarshal(prepared.Receipt.Sources, &refs) != nil {
		return taskProjectionReply("unavailable", "source_versions_unavailable", nil)
	}
	var version json.RawMessage
	for _, ref := range refs {
		if ref.ID == r.TargetID && ref.Source.Kind == "user_memory_record" {
			version = ref.Source.Version
			break
		}
	}
	if len(version) == 0 {
		return taskProjectionReply("error", "target_not_in_current_evidence", nil)
	}
	proof := map[string]any{"projection_id": state.ID, "revision": state.Revision, "session_id": state.Session, "task_id": state.TaskID, "principal": state.Principal, "kind": item.Kind, "content": item.Text, "expires_at": state.ExpiresAt, "target_version": version, "sources": prepared.Receipt.Sources}
	raw, e := json.Marshal(proof)
	if e != nil {
		return 0, nil, e
	}
	digest := taskDigest(raw)
	proof["preview_digest"] = digest
	raw, e = json.Marshal(proof)
	if e != nil {
		return 0, nil, e
	}
	if r.Operation == "promote" {
		if r.PreviewDigest != digest {
			return taskProjectionReply("conflict", "promotion_preview_changed", nil)
		}
		if len(state.PendingPromotion) > 0 && string(state.PendingPromotion) != string(raw) {
			return taskProjectionReply("conflict", "different_promotion_pending", nil)
		}
		state.PendingPromotion = raw
		if e = save(); e != nil {
			return 0, nil, e
		}
	}
	reply, e := json.Marshal(map[string]any{"status": "ok", "admission": "review_required", "promotion": proof, "preview_digest": digest, "scope": state.Binding, "reviewer": "authenticated_owner_user"})
	return 0, []string{string(reply)}, e
}

// Keep admission identity for reconciliation, never a second copy of a draft or
// canonical text that could bypass fresh evidence checks or rendering budgets.
func taskPromotionReceipt(raw json.RawMessage) json.RawMessage {
	if len(raw) == 0 {
		return nil
	}
	var source map[string]json.RawMessage
	if json.Unmarshal(raw, &source) != nil {
		return nil
	}
	out := map[string]json.RawMessage{}
	for _, key := range []string{"status", "kind", "state", "preview_digest", "projection_id", "projection_revision"} {
		if value := source[key]; len(value) > 0 {
			out[key] = value
		}
	}
	var proposal map[string]json.RawMessage
	if json.Unmarshal(source["proposal"], &proposal) == nil {
		ref := map[string]json.RawMessage{}
		for _, key := range []string{"proposal_id", "payload_digest", "state", "decision_id", "review_commit_id"} {
			if value := proposal[key]; len(value) > 0 {
				ref[key] = value
			}
		}
		out["proposal"], _ = json.Marshal(ref)
	}
	result, _ := json.Marshal(out)
	return result
}
