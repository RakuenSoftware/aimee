package memory

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Drafts are deliberately not Records: neither recall nor extraction accepts one.
type correctionDraft struct {
	SchemaVersion int     `json:"schema_version"`
	Content       string  `json:"content"`
	Confidence    float64 `json:"confidence"`
	SessionID     string  `json:"session_id"`
	Tier          string  `json:"tier"`
	UseCases      string  `json:"use_cases"`
	EpistemicKind string  `json:"epistemic_kind"`
}

type correctionProposal struct {
	Replayed          bool                 `json:"replayed,omitempty"`
	ID                string               `json:"proposal_id"`
	Target            MemoryRecordVersion  `json:"target_version"`
	Digest            string               `json:"payload_digest"`
	State             string               `json:"state"`
	Actor             string               `json:"proposer"`
	OriginAuthority   string               `json:"origin_authority"`
	RevisionAuthority string               `json:"revision_authority"`
	Reviewer          string               `json:"reviewer,omitempty"`
	ReviewAuthority   string               `json:"review_authority"`
	DecisionID        string               `json:"decision_id,omitempty"`
	CommitID          string               `json:"review_commit_id,omitempty"`
	Result            *MemoryRecordVersion `json:"result_version,omitempty"`
	Draft             *correctionDraft     `json:"draft,omitempty"`
}

type correctionProposedError struct{ Proposal correctionProposal }

func (e *correctionProposedError) Error() string { return errMutationReviewRequired.Error() }
func (e *correctionProposedError) Unwrap() error { return errMutationReviewRequired }
func proposedCorrection(err error) *correctionProposal {
	var proposed *correctionProposedError
	if errors.As(err, &proposed) {
		return &proposed.Proposal
	}
	return nil
}

const correctionProposalReferenceColumns = `proposal_id::text,owner_id::text,target_id::text,target_revision::text,
 payload_digest,state,actor_principal,reviewer_principal,decision_id,COALESCE(review_commit_id,''),result_id::text,result_revision::text`

const correctionProposalColumns = correctionProposalReferenceColumns + `,payload`

func scanCorrectionProposal(row store.Row, includeDraft ...bool) (correctionProposal, error) {
	p := correctionProposal{Target: MemoryRecordVersion{SchemaVersion: 1}, OriginAuthority: "model", RevisionAuthority: "model", ReviewAuthority: "none"}
	var id, revision, payload string
	withDraft := len(includeDraft) == 0 || includeDraft[0]
	destinations := []any{&p.ID, &p.Target.OwnerID, &p.Target.RecordID, &p.Target.RecordRevision, &p.Digest, &p.State, &p.Actor, &p.Reviewer, &p.DecisionID, &p.CommitID, &id, &revision}
	if withDraft {
		destinations = append(destinations, &payload)
	}
	err := row.Scan(destinations...)
	if err != nil {
		if store.IsNoRows(err) {
			err = ErrMemoryNotFound
		}
		return p, err
	}
	if p.Reviewer != "" {
		p.ReviewAuthority = "user"
	}
	if id != "0" {
		p.Result = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: p.Target.OwnerID, RecordID: id, RecordRevision: revision}
	}
	if !withDraft {
		return p, nil
	}
	p.Draft = &correctionDraft{}
	if err = json.Unmarshal([]byte(payload), p.Draft); err != nil || retryHash([]byte(payload)) != p.Digest {
		return correctionProposal{}, errors.New("memory: invalid correction proposal payload")
	}
	return p, nil
}

// Called with the target row already locked by canonical admission. Deduplicate
// against terminal decisions too: repeating a rejected draft cannot reopen it.
func (s *postgresDataStore) proposeKBCorrection(ctx context.Context, id int64, content string, confidence float64, session, epistemic, tier, useCases string, metadata *DataRequest) error {
	if metadata != nil {
		tier, useCases = metadata.Tier, metadata.UseCases
		if metadata.EpistemicKind != "" {
			epistemic = metadata.EpistemicKind
		}
	}
	var err error
	useCases, err = screenMemoryText(useCases)
	if err != nil {
		return err
	}
	confidence = math.Min(confidence, .8)
	if tier == "L5" {
		confidence = math.Min(confidence, .5)
	}
	draft := correctionDraft{1, content, confidence, session, tier, useCases, epistemic}
	raw, err := json.Marshal(draft)
	if err != nil {
		return err
	}
	if len(raw) > maxDataBody-32768 {
		return errors.New("memory: correction draft exceeds review capacity")
	}
	digest := retryHash(raw)
	p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `INSERT INTO memory_correction_proposals(owner_id,target_id,target_revision,actor_principal,payload,payload_digest)
 SELECT o.owner_id,m.id,m.record_revision,COALESCE(NULLIF(current_setting('aimee.principal',true),''),'system:model-inference'),$2,$3
 FROM memories m CROSS JOIN memory_collection_owner o WHERE m.id=$1 AND o.id=1
 ON CONFLICT(owner_id,target_id,target_revision,payload_digest) DO NOTHING
 RETURNING `+correctionProposalReferenceColumns, id, string(raw), digest), false)
	if errors.Is(err, ErrMemoryNotFound) {
		p, err = scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalReferenceColumns+` FROM memory_correction_proposals
 WHERE target_id=$1 AND owner_id=(SELECT owner_id FROM memory_collection_owner WHERE id=1)
 AND target_revision=(SELECT record_revision FROM memories WHERE id=$1)
 AND payload_digest=$2`, id, digest), false)
	}
	if err != nil {
		return err
	}
	p.Draft = nil // Refusal envelopes carry only the linked review reference.
	return &correctionProposedError{Proposal: p}
}

type correctionReviewRequest struct {
	ProposalID string              `json:"proposal_id"`
	Digest     string              `json:"payload_digest"`
	Action     string              `json:"action"`
	Expected   MemoryRecordVersion `json:"expected_version"`
}

func validProposalID(id string) bool {
	v := MemoryRecordVersion{SchemaVersion: 1, OwnerID: id, RecordID: "1", RecordRevision: "1"}
	return v.validFor(1)
}
func (r *correctionReviewRequest) valid() bool {
	if r == nil || !validProposalID(r.ProposalID) || len(r.Digest) != 64 || strings.ToLower(r.Digest) != r.Digest || (r.Action != "approve" && r.Action != "reject") {
		return false
	}
	_, err := hex.DecodeString(r.Digest)
	id, idErr := strconv.ParseInt(r.Expected.RecordID, 10, 64)
	return err == nil && idErr == nil && r.Expected.validFor(id)
}

var errCorrectionReviewConflict = errors.New("memory: correction review no longer matches the draft, target or decision")

func (s *postgresDataStore) reviewKBCorrection(ctx context.Context, r correctionReviewRequest, caller *bus.CommandContext) (correctionProposal, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !r.valid() || !verifiedRetryCaller(caller) || !caller.UserAuthority {
		return correctionProposal{}, errors.New("memory: authenticated user review transaction required")
	}
	// Resolve the immutable target and lock only its parent in one query.
	// Proposal creation, review and erasure all take parent before proposal.
	var id int64
	var owner, revision, lifecycle, epistemic string
	if err := s.db.QueryRow(ctx, `SELECT m.id,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 m.record_revision::text,m.lifecycle_state,m.epistemic_kind
 FROM memories m JOIN memory_correction_proposals p ON p.target_id=m.id
 WHERE p.proposal_id=$1::uuid FOR UPDATE OF m`, r.ProposalID).Scan(&id, &owner, &revision, &lifecycle, &epistemic); err != nil {
		if store.IsNoRows(err) {
			err = ErrMemoryNotFound
		}
		return correctionProposal{}, err
	}

	p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalColumns+` FROM memory_correction_proposals WHERE proposal_id=$1::uuid FOR UPDATE`, r.ProposalID))
	if err != nil {
		return p, err
	}
	if p.Digest != r.Digest || p.Target != r.Expected || owner != p.Target.OwnerID {
		return p, errCorrectionReviewConflict
	}
	if p.State != "pending" {
		expected := "approved"
		if r.Action == "reject" {
			expected = "rejected"
		}
		if p.State != expected {
			return p, errCorrectionReviewConflict
		}
		if err = s.checkCorrectionResult(ctx, p); err != nil {
			return p, err
		}
		p.Replayed = true
		return p, nil
	}
	if r.Action == "approve" && (revision != p.Target.RecordRevision || lifecycle != "active") {
		return p, errMutationVersionConflict
	}
	var correction preparedKBCorrection
	if r.Action == "approve" {
		d := p.Draft
		if d.SchemaVersion != 1 || !validEpistemicKinds[d.EpistemicKind] || d.Content == "" || d.Confidence < 0 || d.Confidence > .8 {
			return p, errCorrectionReviewConflict
		}
		if d.EpistemicKind != epistemic {
			switch d.EpistemicKind {
			case "episode", "experience", "instruction", "policy":
				return p, errRequiresRevocation
			}
		}
		// User authority admits the transition. It does not author the draft.
		correction, err = s.prepareKBCorrection(ctx, id, d.Content, &d.Confidence, d.SessionID, AuthorityUser,
			&DataRequest{Tier: d.Tier, UseCases: d.UseCases, EpistemicKind: epistemic}, &r.Expected)
		if err != nil {
			return p, err
		}
		if correction.content != d.Content || (d.Tier == "L5" && d.Confidence > .5) {
			return p, errCorrectionReviewConflict
		}
		correction.provenance, correction.ceiling, correction.epistemic = "reviewed_model", .8, d.EpistemicKind
	}
	var previous string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.changeset_id',true),'')`).Scan(&previous); err != nil {
		return p, err
	}
	actor := FactActor{Principal: caller.Principal, Role: "user", Rank: 30, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
	if actor.TransportIdentity == "" {
		actor.TransportIdentity = actor.Principal
	}
	commit, err := s.openFactCommit(ctx, actor, "memory.correction_review", p.ID)
	if err != nil {
		return p, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, commit); err != nil {
		return p, err
	}
	var resultID, resultRevision int64
	decision := "reject"
	state := "rejected"
	if r.Action == "approve" {
		record, err := s.applyKBCorrection(ctx, correction)
		if err != nil {
			return p, err
		}
		resultID = record.ID
		if err = s.captureStoredFactActor(ctx, resultID, AuthorityModel, nil); err != nil {
			return p, err
		}
		if err = s.db.QueryRow(ctx, `SELECT record_revision FROM memories WHERE id=$1`, resultID).Scan(&resultRevision); err != nil {
			return p, err
		}
		decision, state = "accept", "approved"
	}
	decisionID := commit + ":review"
	evidence, _ := json.Marshal(map[string]any{"proposal_id": p.ID, "payload_digest": p.Digest, "target_version": p.Target, "origin_authority": "model", "revision_authority": "model"})
	_, err = s.db.Exec(ctx, `INSERT INTO knowledge_review_decisions(decision_id,item_id,source_queue,decision,authenticated_actor,requested_value,evidence_snapshot,resulting_authority,changeset_id,preview_token,head_at_decision,created_at)
 VALUES($1,$2,'memory_correction',$3,$4,$5,$6,'model;reviewed-by-user',$7,$5,$8,pg_now_text())`, decisionID, p.ID, decision, caller.Principal, p.Digest, string(evidence), commit, p.Target.RecordRevision)
	if err != nil {
		return p, err
	}
	_, err = s.db.Exec(ctx, `UPDATE memory_correction_proposals SET state=$2,reviewer_principal=$3,decision_id=$4,review_commit_id=$5,result_id=$6,result_revision=$7 WHERE proposal_id=$1::uuid`, p.ID, state, caller.Principal, decisionID, commit, resultID, resultRevision)
	if err != nil {
		return p, err
	}
	if err = s.closeFactObjectCommit(ctx, commit, p.ID); err != nil {
		return p, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, previous); err != nil {
		return p, err
	}
	p.State, p.Reviewer, p.ReviewAuthority, p.DecisionID, p.CommitID = state, caller.Principal, "user", decisionID, commit
	if resultID > 0 {
		p.Result = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: strconv.FormatInt(resultID, 10), RecordRevision: strconv.FormatInt(resultRevision, 10)}
	}
	return p, nil
}

func (s *postgresDataStore) listCorrectionProposals(ctx context.Context, id string, limit int) ([]correctionProposal, error) {
	if limit < 1 || limit > 100 {
		return nil, errors.New("memory: invalid proposal limit")
	}
	query := `SELECT ` + correctionProposalReferenceColumns + ` FROM memory_correction_proposals ORDER BY created_at DESC,proposal_id LIMIT $1`
	parameters := []any{limit}
	if id != "" {
		query = `SELECT ` + correctionProposalColumns + ` FROM memory_correction_proposals WHERE proposal_id=$1::uuid LIMIT 1`
		parameters = []any{id}
	}
	rows, err := s.db.Query(ctx, query, parameters...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []correctionProposal{}
	for rows.Next() {
		p, err := scanCorrectionProposal(rows, id != "")
		if err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

func (s *postgresDataStore) checkCorrectionResult(ctx context.Context, p correctionProposal) error {
	if p.Result == nil {
		return nil
	}
	id, _ := strconv.ParseInt(p.Result.RecordID, 10, 64)
	record, err := s.getAtVersioned(ctx, Scope{}, id, false, "", true)
	if errors.Is(err, ErrMemoryNotFound) {
		return errReplayUnavailable
	}
	if err != nil {
		return err
	}
	if *record.Version != *p.Result {
		return errReplayUnavailable
	}
	return nil
}
