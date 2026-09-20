package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// The caller already holds the canonical parent lock and host actor context.
// Draft identity includes the parent version and payload, including terminal
// decisions, so repeating a rejected suggestion cannot reopen it.
func (s *postgresDataStore) proposePersonalCorrection(ctx context.Context, old, wanted Record) error {
	draft := correctionDraft{SchemaVersion: 1, Content: wanted.Content, Confidence: wanted.Confidence, Tier: wanted.Tier, EpistemicKind: old.Kind}
	raw, err := json.Marshal(draft)
	if err != nil {
		return err
	}
	if len(raw) > maxDataBody-32768 {
		return errors.New("memory: correction draft exceeds review capacity")
	}
	digest := retryHash(raw)
	p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `INSERT INTO user_memory_correction_proposals(owner_id,target_id,target_revision,actor_principal,actor_transport,payload,payload_digest)
 SELECT o.owner_id,m.id,m.record_revision,current_setting('aimee.private_principal'),current_setting('aimee.private_transport'),$2,$3
 FROM user_memories m CROSS JOIN user_memory_collection_generation o WHERE m.id=$1 AND o.id=1
 ON CONFLICT(owner_id,target_id,target_revision,payload_digest) DO NOTHING
 RETURNING `+correctionProposalReferenceColumns, old.ID, string(raw), digest), false)
	if errors.Is(err, ErrMemoryNotFound) {
		p, err = scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalReferenceColumns+` FROM user_memory_correction_proposals
 WHERE target_id=$1 AND owner_id=(SELECT owner_id FROM user_memory_collection_generation WHERE id=1)
 AND target_revision=(SELECT record_revision FROM user_memories WHERE id=$1) AND payload_digest=$2`, old.ID, digest), false)
	}
	if err != nil {
		return err
	}
	return &correctionProposedError{Proposal: p}
}

func (s *postgresDataStore) listPersonalCorrectionProposals(ctx context.Context, id string, limit int) ([]correctionProposal, error) {
	if limit < 1 || limit > 100 {
		return nil, errors.New("memory: invalid proposal limit")
	}
	columns := correctionProposalReferenceColumns
	predicate := ""
	args := []any{limit}
	bound := "$1"
	if id != "" {
		columns = correctionProposalColumns
		predicate = " AND proposal_id=$1::uuid"
		args = []any{id}
		bound = "1"
	}
	rows, err := s.db.Query(ctx, `SELECT `+columns+` FROM user_memory_correction_proposals
 WHERE owner_id=(SELECT owner_id FROM user_memory_collection_generation WHERE id=1)
 AND EXISTS(SELECT 1 FROM user_memories m WHERE m.id=target_id AND m.lifecycle_state IN ('active','retired')
 AND (m.valid_until IS NULL OR m.valid_until>now()))`+predicate+` ORDER BY created_at DESC,proposal_id LIMIT `+bound, args...)
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

func (s *postgresDataStore) reviewPersonalCorrection(ctx context.Context, r correctionReviewRequest, caller *bus.CommandContext) (correctionProposal, error) {
	if s.placement != PlacementServer || !r.valid() || !verifiedRetryCaller(caller) || !caller.UserAuthority {
		return correctionProposal{}, errors.New("memory: authenticated user review required")
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return correctionProposal{}, err
		}
		defer tx.Rollback(context.WithoutCancel(ctx))
		bound := *s
		bound.db = tx
		p, err := bound.reviewPersonalCorrection(ctx, r, caller)
		if err != nil {
			return p, err
		}
		if err = tx.Commit(ctx); err != nil {
			return correctionProposal{}, err
		}
		return p, nil
	}
	if _, ok := s.db.(store.Tx); !ok {
		return correctionProposal{}, errors.New("memory: private review requires a transaction")
	}
	var id int64
	var owner, revision, state, kind string
	var current bool
	err := s.db.QueryRow(ctx, `SELECT m.id,(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1),
 m.record_revision::text,m.lifecycle_state,m.kind,(m.valid_until IS NULL OR m.valid_until>now())
 FROM user_memories m JOIN user_memory_correction_proposals p ON p.target_id=m.id
 WHERE p.proposal_id=$1::uuid AND m.lifecycle_state IN ('active','retired') FOR NO KEY UPDATE OF m`, r.ProposalID).Scan(&id, &owner, &revision, &state, &kind, &current)
	if store.IsNoRows(err) {
		err = ErrMemoryNotFound
	}
	if err != nil {
		return correctionProposal{}, err
	}
	p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalColumns+` FROM user_memory_correction_proposals WHERE proposal_id=$1::uuid FOR UPDATE`, r.ProposalID))
	if err != nil {
		return p, err
	}
	if p.Digest != r.Digest || p.Target != r.Expected || p.Target.OwnerID != owner {
		return p, errCorrectionReviewConflict
	}
	desired := "approved"
	if r.Action == "reject" {
		desired = "rejected"
	}
	if p.State != "pending" {
		if p.State != desired {
			return p, errCorrectionReviewConflict
		}
		if !current {
			return p, errReplayUnavailable
		}
		if err = s.checkCorrectionResult(ctx, p); err != nil {
			return p, err
		}
		p.Replayed = true
		return p, nil
	}
	if !current {
		return p, ErrMemoryNotFound
	}
	if r.Action == "approve" {
		if state != "active" || revision != p.Target.RecordRevision {
			return p, errMutationVersionConflict
		}
		d := p.Draft
		if d.SchemaVersion != 1 || d.Content == "" || d.EpistemicKind != kind || d.Confidence < 0 || d.Confidence > .8 || (d.Tier == "L5" && d.Confidence > .5) {
			return p, errCorrectionReviewConflict
		}
		if err = admitMemoryReplacement(kind, "unknown", AuthorityUser); err != nil {
			return p, err
		}
		screened, err := screenMemoryText(d.Content)
		if err != nil {
			return p, err
		}
		if screened != d.Content {
			return p, errCorrectionReviewConflict
		}
	}
	_, err = s.db.Exec(ctx, `SELECT set_config('aimee.private_authority','review',true),
 set_config('aimee.private_review_proposal',$1,true),set_config('aimee.private_reviewer',$2,true),set_config('aimee.private_review_transport',$3,true)`, p.ID, caller.Principal, caller.TransportIdentity)
	if err != nil {
		return p, err
	}
	var resultID, resultRevision int64
	if r.Action == "approve" {
		d := p.Draft
		err = s.db.QueryRow(ctx, `UPDATE user_memories SET content=$2,tier=$3,confidence=$4,updated_at=now() WHERE id=$1 RETURNING id,record_revision`, id, d.Content, d.Tier, d.Confidence).Scan(&resultID, &resultRevision)
		if err != nil {
			return p, err
		}
	}
	var decision string
	err = s.db.QueryRow(ctx, `WITH decision AS (SELECT gen_random_uuid()::text AS id)
 UPDATE user_memory_correction_proposals SET state=$2,reviewer_principal=$3,reviewer_transport=$4,
 decision_id=decision.id,review_commit_id=decision.id,result_id=$5,result_revision=$6
 FROM decision WHERE proposal_id=$1::uuid RETURNING decision_id`, p.ID, desired, caller.Principal, caller.TransportIdentity, resultID, resultRevision).Scan(&decision)
	if err != nil {
		return p, err
	}
	p.State, p.Reviewer, p.ReviewAuthority, p.DecisionID, p.CommitID = desired, caller.Principal, "user", decision, decision
	if resultID > 0 {
		p.Result = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: strconv.FormatInt(resultID, 10), RecordRevision: strconv.FormatInt(resultRevision, 10)}
	}
	return p, nil
}
