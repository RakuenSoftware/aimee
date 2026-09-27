package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Issued by the host's task revision owner. It can only enter through the
// internal runtime operation, and creates a draft in the existing review owner.
// A projection is never inserted as an authoritative memory record.
type taskPromotion struct {
	ProjectionID  string               `json:"projection_id"`
	Revision      string               `json:"revision"`
	SessionID     string               `json:"session_id"`
	TaskID        string               `json:"task_id"`
	Principal     string               `json:"principal"`
	Kind          string               `json:"kind"`
	Content       string               `json:"content"`
	Expires       time.Time            `json:"expires_at"`
	Target        MemoryRecordVersion  `json:"target_version"`
	Sources       []typedProjectionRef `json:"sources"`
	PreviewDigest string               `json:"preview_digest"`
}

func (p *taskPromotion) valid() bool {
	if p == nil || !releaseTokenValid(p.ProjectionID) || p.SessionID == "" || p.Principal == "" || p.Content == "" || len(p.Content) > 8192 || (p.Kind != "hypothesis" && p.Kind != "working_decision") || p.Expires.IsZero() {
		return false
	}
	for _, v := range []string{p.Revision, p.TaskID} {
		n, e := strconv.ParseUint(v, 10, 63)
		if e != nil || n == 0 || strconv.FormatUint(n, 10) != v {
			return false
		}
	}
	id, e := strconv.ParseInt(p.Target.RecordID, 10, 64)
	return e == nil && p.Target.validFor(id) && (&sourceRevalidation{SchemaVersion: 1, CheckID: p.ProjectionID, Sources: p.Sources}).valid()
}
func handleTaskPromotion(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || options.placement != PlacementServer || !verifiedRetryCaller(options.commandContext) {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	var p taskPromotion
	if json.Unmarshal(args["promotion"], &p) != nil || !p.valid() {
		return commandResult(commandError("invalid_argument", "invalid task promotion"))
	}
	if p.Principal != options.commandContext.Principal && p.Principal != options.commandContext.TransportIdentity {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{Operation: "task-projection-propose", Scope: Scope{Type: ScopeUser}, TaskPromotion: &p}
	raw, _ := json.Marshal(request)
	body, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(body, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
func (s *postgresDataStore) checkTaskPromotionSources(ctx context.Context, p *taskPromotion, atAdmission bool) error {
	if !p.valid() || s.placement != PlacementServer {
		return errors.New("memory: invalid task promotion proof")
	}
	// This owner has no task SQL. Serialize canonical writers before checking the
	// complete source set; the lock is retained through proposal/review commit.
	if _, e := s.db.Exec(ctx, `LOCK TABLE user_memories IN SHARE ROW EXCLUSIVE MODE`); e != nil {
		return e
	}
	if atAdmission {
		var nowMicros int64
		if e := s.db.QueryRow(ctx, `SELECT floor(extract(epoch FROM clock_timestamp())*1000000)::bigint`).Scan(&nowMicros); e != nil {
			return e
		}
		if !time.UnixMicro(nowMicros).Before(p.Expires) {
			return errMutationVersionConflict
		}
	}
	valid, e := s.revalidatePersonalSources(ctx, &sourceRevalidation{SchemaVersion: 1, CheckID: p.ProjectionID, SendGuard: "acquire", Sources: p.Sources})
	if e != nil {
		return e
	}
	if !valid {
		return errMutationVersionConflict
	}
	return nil
}
func (s *postgresDataStore) proposeTaskProjection(ctx context.Context, p *taskPromotion, caller *bus.CommandContext) (map[string]any, error) {
	if !verifiedRetryCaller(caller) || caller.ScopeKind != "" || (p.Principal != caller.Principal && p.Principal != caller.TransportIdentity) {
		return commandError("unauthorized", "authenticated task owner required"), nil
	}
	if e := s.checkTaskPromotionSources(ctx, p, true); e != nil {
		if errors.Is(e, errMutationVersionConflict) {
			return commandError("conflict", "task promotion sources changed"), nil
		}
		return nil, e
	}
	id, _ := strconv.ParseInt(p.Target.RecordID, 10, 64)
	old, e := s.getAtVersioned(ctx, Scope{Type: ScopeUser}, id, false, "", true)
	if e != nil {
		return nil, e
	}
	if old.Version == nil || *old.Version != p.Target {
		return commandError("conflict", "target revision changed"), nil
	}
	content, e := screenMemoryText("Task " + p.Kind + ": " + p.Content)
	if e != nil {
		return nil, e
	}
	wanted := old
	wanted.Content = content
	wanted.Confidence = .5
	if _, e = s.db.Exec(ctx, `SELECT set_config('aimee.private_authority','model',true),set_config('aimee.private_principal',$1,true),set_config('aimee.private_transport',$2,true)`, caller.Principal, caller.TransportIdentity); e != nil {
		return nil, e
	}
	e = s.proposePersonalCorrection(ctx, old, wanted, p)
	if proposal := proposedCorrection(e); proposal != nil {
		return map[string]any{"status": "ok", "store": "user", "admission": "review_required", "proposal": proposal, "projection_id": p.ProjectionID, "projection_revision": p.Revision}, nil
	}
	return nil, e
}

// Read an immutable pending draft before row locks so all task reviews acquire
// the collection write fence in the same order. Ordinary drafts are unchanged.
func (s *postgresDataStore) checkTaskPromotionReview(ctx context.Context, id string) error {
	var raw string
	e := s.db.QueryRow(ctx, `SELECT payload FROM user_memory_correction_proposals WHERE proposal_id=$1::uuid AND state='pending' AND payload::jsonb ? 'task_projection'`, id).Scan(&raw)
	if store.IsNoRows(e) {
		return nil
	}
	if e != nil {
		return e
	}
	var draft correctionDraft
	if json.Unmarshal([]byte(raw), &draft) != nil {
		return errCorrectionReviewConflict
	}
	return s.checkTaskPromotionSources(ctx, draft.TaskProjection, false)
}
