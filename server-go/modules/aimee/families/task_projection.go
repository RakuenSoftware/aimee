package families

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const taskProjectionPolicy = "task-projection-v1"

type taskProjectionItem struct {
	Kind        string `json:"kind"`
	Text        string `json:"text"`
	EventID     string `json:"event_id,omitempty"`
	EventDigest string `json:"event_digest,omitempty"`
}
type taskProjectionBinding struct {
	Store   string `json:"store"`
	Project string `json:"project"`
	Task    string `json:"task"`
	View    string `json:"view"`
}
type taskProjectionState struct {
	MemoryGaps       json.RawMessage       `json:"memory_gaps"`
	Coverage         json.RawMessage       `json:"coverage"`
	PendingPromotion json.RawMessage       `json:"pending_promotion,omitempty"`
	LastPromotion    json.RawMessage       `json:"last_promotion,omitempty"`
	SchemaVersion    int                   `json:"schema_version"`
	ID               string                `json:"projection_id"`
	Class            string                `json:"class"`
	Policy           string                `json:"policy"`
	Principal        string                `json:"principal"`
	Session          string                `json:"session_id"`
	TaskID           string                `json:"task_id"`
	Revision         string                `json:"revision"`
	State            string                `json:"state"`
	Binding          taskProjectionBinding `json:"binding"`
	CreatedAt        time.Time             `json:"created_at"`
	ExpiresAt        time.Time             `json:"expires_at"`
	Items            []taskProjectionItem  `json:"items"`
	Prepared         json.RawMessage       `json:"dependencies,omitempty"`
	EvidenceIdentity string                `json:"evidence_identity"`
	Events           []string              `json:"incorporated_events"`
	Gaps             []string              `json:"gaps"`
	Replay           string                `json:"replay_availability"`
}
type taskProjectionRequest struct {
	ClaimIndex      *int                  `json:"claim_index"`
	TargetID        string                `json:"target_id"`
	PreviewDigest   string                `json:"preview_digest"`
	PromotionResult json.RawMessage       `json:"promotion_result"`
	Operation       string                `json:"operation"`
	TaskID          string                `json:"task_id"`
	Expected        string                `json:"expected_revision"`
	Binding         taskProjectionBinding `json:"binding"`
	TTL             int                   `json:"ttl_seconds"`
	Budget          *int                  `json:"max_context_bytes"`
	Items           []taskProjectionItem  `json:"items"`
	Events          []string              `json:"events"`
	// Only the host bridge supplies this after calling the Go memory owner.
	Prepared json.RawMessage `json:"prepared"`
}

type taskPreparedView struct {
	Omissions json.RawMessage `json:"omissions"`
	Coverage  json.RawMessage `json:"coverage"`
	Status    string          `json:"status"`
	View      string          `json:"view"`
	Store     string          `json:"store"`
	Rendered  string          `json:"rendered_context"`
	Cache     struct {
		Identity    string `json:"identity"`
		Revalidated bool   `json:"owner_revalidated"`
	} `json:"cache"`
	Receipt struct {
		Sources json.RawMessage `json:"sources"`
		Hash    string          `json:"payload_sha256"`
	} `json:"receipt"`
}

func taskDigest(raw []byte) string { return fmt.Sprintf("sha256:%x", sha256.Sum256(raw)) }

// A lost admission reply must not pin disposable state forever. Retain only
// the immutable proof digest for reconciliation; canonical proposals remain
// owned by the durable review system.
func (state *taskProjectionState) releasePending() {
	if len(state.PendingPromotion) == 0 {
		return
	}
	var proof struct {
		Digest string `json:"preview_digest"`
	}
	_ = json.Unmarshal(state.PendingPromotion, &proof)
	state.LastPromotion, _ = json.Marshal(map[string]string{"state": "submission_unknown_reconciliation_required", "preview_digest": proof.Digest})
	state.PendingPromotion = nil
}
func taskProjectionReply(status, reason string, state *taskProjectionState) (uint32, []string, error) {
	result := map[string]any{"status": status, "reason": reason, "class": "derived_non_authoritative", "independent_support": false}
	if state != nil {
		result["projection"] = state
	}
	raw, err := json.Marshal(result)
	return store.StatusOK, []string{string(raw)}, err
}
func taskEvent(ctx context.Context, q store.Queryer, sid, id string) (taskProjectionItem, error) {
	n, err := strconv.ParseInt(id, 10, 64)
	if err != nil || n < 1 || strconv.FormatInt(n, 10) != id {
		return taskProjectionItem{}, fmt.Errorf("invalid execution event")
	}
	var tool, input, result string
	err = q.QueryRow(ctx, `SELECT tool_name,tool_input,tool_result FROM conv_tool_events WHERE session_id=$1 AND id=$2`, sid, n).Scan(&tool, &input, &result)
	if err != nil {
		return taskProjectionItem{}, err
	}
	raw, _ := json.Marshal([]string{tool, input, result})
	// Preserve complete labels; an oversized event is a gap, never truncated into
	// an apparently complete account of execution.
	if len(result) > 8192 {
		return taskProjectionItem{}, fmt.Errorf("execution observation exceeds projection bound")
	}
	return taskProjectionItem{Kind: "execution_observation", Text: "Recorded result of " + tool + ": " + result, EventID: id, EventDigest: taskDigest(raw)}, nil
}

// This operation is host-only. The session owner locks runtime state and uses
// CAS; memory validity/selection comes exclusively from the Go memory owner.
// No canonical memory SQL or writable copy of a canonical claim exists here.
func sessionTaskProjectionApply(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	principal, sid := f[0], f[1]
	if principal == "" || sid == "" || len(f[2]) > 131072 {
		return store.StatusInvalid, nil, nil
	}
	var request taskProjectionRequest
	if err := json.Unmarshal([]byte(f[2]), &request); err != nil {
		return store.StatusInvalid, nil, nil
	}
	budget := 16384
	if request.Budget != nil {
		budget = *request.Budget
	}
	if budget < 0 || budget > 65536 {
		return store.StatusInvalid, nil, nil
	}
	task, err := strconv.ParseInt(request.TaskID, 10, 64)
	if err != nil || task < 1 || strconv.FormatInt(task, 10) != request.TaskID {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	if err = q.QueryRow(ctx, `SELECT principal FROM server_sessions WHERE id=$1 FOR UPDATE`, sid).Scan(&owner); store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	} else if err != nil {
		return 0, nil, err
	}
	if owner != principal {
		return store.StatusInvalid, nil, nil
	}
	var active int64
	var raw string
	var now time.Time
	if err = q.QueryRow(ctx, `SELECT active_task_id,task_projection_state,clock_timestamp() FROM session_state WHERE session_id=$1 FOR UPDATE`, sid).Scan(&active, &raw, &now); store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	} else if err != nil {
		return 0, nil, err
	}
	if active != task {
		return taskProjectionReply("error", "active_task_mismatch", nil)
	}
	state := taskProjectionState{}
	if raw != "" && json.Unmarshal([]byte(raw), &state) != nil {
		return 0, nil, fmt.Errorf("invalid stored task projection")
	}
	if state.ID != "" && (state.Principal != principal || state.Session != sid) {
		return store.StatusInvalid, nil, nil
	}
	if state.TaskID != request.TaskID {
		state = taskProjectionState{}
	}
	save := func() error {
		b, e := json.Marshal(state)
		if e != nil {
			return e
		}
		if len(b) > 131072 {
			return fmt.Errorf("projection state exceeds bound")
		}
		_, e = q.Exec(ctx, `UPDATE session_state SET task_projection_state=$2,updated_at=now() WHERE session_id=$1`, sid, string(b))
		return e
	}
	if state.ID != "" && !now.Before(state.ExpiresAt) && state.State != "discarded" {
		state.State = "expired"
		state.releasePending()
		state.Items = nil
		state.Prepared = nil
		state.Replay = "digest_only_projection_not_retained"
		if err = save(); err != nil {
			return 0, nil, err
		}
	}
	if request.Operation == "describe" {
		if state.ID == "" {
			return taskProjectionReply("missing", "projection_unavailable", nil)
		}
		copy := state
		copy.Items = nil
		copy.Prepared = nil
		copy.PendingPromotion = nil
		return taskProjectionReply("ok", "binding_only", &copy)
	}
	revision := uint64(0)
	if state.Revision != "" {
		revision, err = strconv.ParseUint(state.Revision, 10, 64)
		if err != nil {
			return 0, nil, err
		}
	}
	expected, e := strconv.ParseUint(request.Expected, 10, 64)
	if request.Operation != "get" && (e != nil || strconv.FormatUint(expected, 10) != request.Expected || expected != revision) {
		return taskProjectionReply("conflict", "expected_revision_mismatch", nil)
	}
	if request.Operation == "promotion_finish" {
		var pending map[string]any
		if json.Unmarshal(state.PendingPromotion, &pending) != nil || pending["preview_digest"] != request.PreviewDigest || len(request.PromotionResult) == 0 {
			return taskProjectionReply("conflict", "promotion_receipt_mismatch", nil)
		}
		state.LastPromotion = request.PromotionResult
		state.PendingPromotion = nil
		if err = save(); err != nil {
			return 0, nil, err
		}
		return taskProjectionReply("ok", "promotion_proposal_recorded", nil)
	}
	if len(state.PendingPromotion) > 0 && request.Operation != "get" && request.Operation != "promote" && request.Operation != "promotion_preview" && request.Operation != "discard" {
		return taskProjectionReply("conflict", "promotion_submission_pending", nil)
	}
	if request.Operation == "discard" {
		if state.ID == "" {
			return taskProjectionReply("missing", "projection_unavailable", nil)
		}
		state.releasePending()
		state.State = "discarded"
		state.Revision = strconv.FormatUint(revision+1, 10)
		state.Items = nil
		state.Prepared = nil
		state.Replay = "digest_only_projection_not_retained"
		if err = save(); err != nil {
			return 0, nil, err
		}
		return taskProjectionReply("ok", "derived_state_discarded", &state)
	}
	if request.Operation != "get" && request.Operation != "rebuild" && request.Operation != "promotion_preview" && request.Operation != "promote" {
		return store.StatusInvalid, nil, nil
	}
	if request.Operation != "rebuild" && (state.ID == "" || state.State == "expired" || state.State == "discarded") {
		return taskProjectionReply("unavailable", "projection_"+state.State, nil)
	}
	var prepared taskPreparedView
	if json.Unmarshal(request.Prepared, &prepared) != nil || (prepared.Status != "ok" && prepared.Status != "degraded") || !prepared.Cache.Revalidated || !strings.HasPrefix(prepared.Cache.Identity, "sha256:") || prepared.Receipt.Hash != taskDigest([]byte(prepared.Rendered)) {
		return taskProjectionReply("unavailable", "memory_owner_evidence_unavailable", nil)
	}
	if request.Operation != "rebuild" {
		if state.Policy != taskProjectionPolicy || state.EvidenceIdentity != prepared.Cache.Identity {
			state.State = "stale"
			var previous, current []struct {
				Channel string `json:"channel"`
				ID      string `json:"stable_id"`
			}
			if json.Unmarshal(state.Prepared, &previous) != nil || json.Unmarshal(prepared.Receipt.Sources, &current) != nil {
				state.State = "blocked"
			} else {
				visible := map[string]bool{}
				for _, ref := range current {
					visible[ref.Channel+"/"+ref.ID] = true
				}
				for _, ref := range previous {
					if !visible[ref.Channel+"/"+ref.ID] {
						state.State = "blocked"
						break
					}
				}
			}
			state.releasePending()
			state.Gaps = []string{"memory_dependencies_changed_rebuild_required"}
			state.Items = nil
			state.Prepared = nil
			state.Replay = "digest_only_projection_not_retained"
			if err = save(); err != nil {
				return 0, nil, err
			}
			return taskProjectionReply("unavailable", "projection_"+state.State, nil)
		}
		if state.State != "active" {
			return taskProjectionReply("unavailable", "projection_"+state.State, nil)
		}
		for _, item := range state.Items {
			if item.EventID != "" {
				current, e := taskEvent(ctx, q, sid, item.EventID)
				if e != nil || current.EventDigest != item.EventDigest {
					state.State = "blocked"
					state.releasePending()
					state.Items = nil
					state.Prepared = nil
					state.Replay = "digest_only_projection_not_retained"
					if err = save(); err != nil {
						return 0, nil, err
					}
					return taskProjectionReply("unavailable", "execution_evidence_unavailable", nil)
				}
			}
		}
	} else {
		b := request.Binding
		if (b.Store != "user" && b.Store != "kb") || b.Project == "" || strings.TrimSpace(b.Task) == "" || len(b.Task) > 8192 || b.View != "briefing" || prepared.Store != b.Store || prepared.View != b.View || request.TTL < 1 || request.TTL > 86400 || len(request.Items)+len(request.Events) > 32 {
			return store.StatusInvalid, nil, nil
		}
		items := []taskProjectionItem{}
		for _, item := range request.Items {
			if (item.Kind != "hypothesis" && item.Kind != "planned_action" && item.Kind != "working_decision" && item.Kind != "open_question") || strings.TrimSpace(item.Text) == "" || len(item.Text) > 4096 || item.EventID != "" || item.EventDigest != "" {
				return store.StatusInvalid, nil, nil
			}
			items = append(items, item)
		}
		seen := map[string]bool{}
		for _, id := range request.Events {
			if seen[id] {
				return store.StatusInvalid, nil, nil
			}
			seen[id] = true
			item, e := taskEvent(ctx, q, sid, id)
			if e != nil {
				return taskProjectionReply("unavailable", "execution_evidence_unavailable", nil)
			}
			items = append(items, item)
		}
		if state.ID == "" {
			nonce := make([]byte, 16)
			if _, err = rand.Read(nonce); err != nil {
				return 0, nil, err
			}
			state.ID = hex.EncodeToString(nonce)
			state.CreatedAt = now
		}
		state.SchemaVersion = 1
		state.Class = "derived_non_authoritative"
		state.Policy = taskProjectionPolicy
		state.Principal = principal
		state.Session = sid
		state.TaskID = request.TaskID
		state.Revision = strconv.FormatUint(revision+1, 10)
		state.State = "active"
		state.Binding = b
		state.ExpiresAt = now.Add(time.Duration(request.TTL) * time.Second)
		state.Items = items
		state.Prepared = prepared.Receipt.Sources
		state.MemoryGaps = prepared.Omissions
		state.Coverage = prepared.Coverage
		state.EvidenceIdentity = prepared.Cache.Identity
		state.Events = request.Events
		state.Gaps = []string{}
		state.Replay = "owner_state_until_expiry"
		if err = save(); err != nil {
			return 0, nil, err
		}
	}
	if request.Operation == "promotion_preview" || request.Operation == "promote" {
		return taskProjectionPromotion(&state, request, prepared, save)
	}
	model, _ := json.Marshal(map[string]any{"class": state.Class, "independent_support": false, "memory_evidence": prepared.Rendered, "working_entries": state.Items, "memory_gaps": state.MemoryGaps, "task_sufficiency": "unknown"})
	rendered := "<task_projection trust=untrusted authorization=none>" + string(model) + "</task_projection>"
	gaps := []string{}
	if len(rendered) > budget {
		rendered = ""
		gaps = append(gaps, "coherent_projection_exceeds_budget")
	}
	copy := state
	copy.Prepared = nil
	copy.PendingPromotion = nil
	response := map[string]any{"status": "ok", "projection": copy, "rendered_context": rendered, "omissions": gaps, "receipt": map[string]any{"projection_id": state.ID, "revision": state.Revision, "payload_sha256": taskDigest([]byte(rendered)), "payload_bytes": len(rendered), "sources": prepared.Receipt.Sources, "class": state.Class, "independent_support": false, "replay_availability": state.Replay}}
	result, err := json.Marshal(response)
	return store.StatusOK, []string{string(result)}, err
}
