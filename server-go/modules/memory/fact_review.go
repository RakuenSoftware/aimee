package memory

import (
	"context"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

var errFactReviewConflict = errors.New("memory: fact review conflicts with current state")

type reviewFact struct {
	factState
	Source, Relation, Target, Commit, Kind string
}

const factEvidenceVisible = `(NOT EXISTS(SELECT 1 FROM fact_evidence f WHERE f.assertion_id=e.id AND f.source_kind='memory')
 OR EXISTS(SELECT 1 FROM fact_evidence f JOIN memories m ON f.source_id='memory:'||m.id::text
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND m.lifecycle_state='active' AND m.activation_suppressed=0))`

func (s *postgresDataStore) loadReviewFact(ctx context.Context, id int64) (reviewFact, error) {
	var f reviewFact
	err := s.db.QueryRow(ctx, `SELECT `+factStateColumns+`,source,relation,target,commit_id,epistemic_kind FROM entity_edges e
 WHERE id=$1 AND edge_class='semantic' AND `+factEvidenceVisible+` FOR UPDATE`, id).Scan(&f.ID, &f.Lifecycle, &f.Superseded, &f.Invalidated, &f.Suppressed, &f.Confidence, &f.Rank, &f.Version, &f.Source, &f.Relation, &f.Target, &f.Commit, &f.Kind)
	if store.IsNoRows(err) {
		err = ErrMemoryNotFound
	}
	return f, err
}

func (s *postgresDataStore) reviewFact(ctx context.Context, actor FactActor, id int64, action string) (factMutationResult, error) {
	var result factMutationResult
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !validMutationActor(actor) || actor.Rank != 40 || id <= 0 {
		return result, errors.New("memory: verified operator transaction required")
	}
	if action != "approve" && action != "reject" && action != "undo" {
		return result, errors.New("memory: invalid fact review action")
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return result, err
	}
	before, err := s.loadReviewFact(ctx, id)
	if err != nil {
		return result, err
	}
	next, rank := "promoted", 40
	reverseID := int64(0)
	reverseCommit := ""
	if action == "reject" {
		next = "invalidated"
	}
	if action == "undo" {
		// The latest transition must still be review-owned. A new ingest, correction
		// or independent mutation cannot be erased by undoing an older review.
		var latest string
		err = s.db.QueryRow(ctx, `SELECT commit_id FROM fact_review_actions WHERE assertion_id=$1 ORDER BY id DESC LIMIT 1`, id).Scan(&latest)
		if err != nil || latest != before.Commit {
			return result, errFactReviewConflict
		}
		err = s.db.QueryRow(ctx, `SELECT r.id,r.prior_lifecycle,r.commit_id,
 (SELECT c.before_authority_rank FROM fact_graph_changes c WHERE c.commit_id=r.commit_id AND c.assertion_id=r.assertion_id ORDER BY c.id DESC LIMIT 1)
 FROM fact_review_actions r WHERE r.assertion_id=$1 AND r.action IN ('approve','reject')
 AND NOT EXISTS(SELECT 1 FROM fact_review_actions u WHERE u.reverses_review_id=r.id) ORDER BY r.id DESC LIMIT 1`, id).Scan(&reverseID, &next, &reverseCommit, &rank)
		if store.IsNoRows(err) {
			return result, errFactReviewConflict
		}
		if err != nil {
			return result, err
		}
	}
	input := factAssertion{FactCandidate: FactCandidate{Subject: before.Source, Relation: before.Relation, Object: before.Target}}
	identity, subject := factIdentity(before.Source, before.Relation, before.Target)
	if action == "approve" {
		blocked, err := s.factTombstoned(ctx, input, identity)
		if err != nil {
			return result, err
		}
		if blocked {
			return result, errFactTombstoned
		}
	}
	commit, err := s.openFactCommit(ctx, actor, "fact.review", "")
	if err != nil {
		return result, err
	}
	if action == "approve" && factFunctional(before.Relation) {
		rows, err := s.db.Query(ctx, `SELECT `+factStateColumns+` FROM entity_edges WHERE edge_class='semantic' AND id<>$1
 AND ((identity_subject_key<>'' AND identity_subject_key=$2) OR (identity_subject_key='' AND source=$3 AND relation=$4))
 AND superseded_at='' AND invalidated_at='' AND suppressed=0 AND lifecycle_state IN ('persistent','promoted') ORDER BY id LIMIT 65 FOR UPDATE`, id, subject, before.Source, before.Relation)
		if err != nil {
			return result, err
		}
		priors := []factState{}
		seen := map[int64]bool{}
		for rows.Next() {
			f, e := scanFactState(rows)
			if e != nil {
				rows.Close()
				return result, e
			}
			priors = append(priors, f)
			seen[f.ID] = true
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return result, err
		}
		_, legacy, err := s.legacyFactMatches(ctx, input, identity, subject)
		if err != nil {
			return result, err
		}
		for _, f := range legacy {
			if f.ID != id && !seen[f.ID] && f.Lifecycle != "candidate" {
				priors = append(priors, f)
				seen[f.ID] = true
			}
		}
		if len(priors) > 64 {
			return result, errors.New("memory: review correction exceeds bound")
		}
		for _, prior := range priors {
			after, err := scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state='superseded',superseded_at=pg_now_text(),version=version+1,commit_id=$2 WHERE id=$1 RETURNING `+factStateColumns, prior.ID, commit))
			if err != nil {
				return result, err
			}
			if err = s.recordFactChange(ctx, commit, "supersede", "operator-approved correction", prior, after); err != nil {
				return result, err
			}
		}
	}
	if action == "undo" {
		if _, err = s.db.Exec(ctx, `UPDATE memory_rejection_tombstones SET active=0,restored_at=pg_now_text(),restored_by=$4
 WHERE object_kind='fact' AND active=1 AND source=$1 AND relation=$2 AND target=$3`, before.Source, before.Relation, before.Target, actor.Principal); err != nil {
			return result, err
		}
	}
	after, err := scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state=$2,
 invalidated_at=CASE WHEN $2='invalidated' THEN pg_now_text() ELSE '' END,authority_rank=$3,actor_principal=$4,version=version+1,commit_id=$5
 WHERE id=$1 RETURNING `+factStateColumns, id, next, rank, actor.Principal, commit))
	if err != nil {
		return result, err
	}
	if err = s.recordFactChange(ctx, commit, action, "operator review", before.factState, after); err != nil {
		return result, err
	}
	if action == "reject" {
		if _, err = s.db.Exec(ctx, `INSERT INTO memory_rejection_tombstones(object_kind,source,relation,target,authority_rank,reason,rejected_by)
 VALUES('fact',$1,$2,$3,40,'operator review rejection',$4) ON CONFLICT DO NOTHING`, before.Source, before.Relation, before.Target, actor.Principal); err != nil {
			return result, err
		}
	}
	if action == "undo" {
		rows, err := s.db.Query(ctx, `SELECT assertion_id,before_lifecycle,before_superseded_at,before_invalidated_at,before_suppressed,before_confidence,before_authority_rank,before_version
 FROM fact_graph_changes WHERE commit_id=$1 AND assertion_id<>$2 AND assertion_id>0 ORDER BY id DESC LIMIT 65`, reverseCommit, id)
		if err != nil {
			return result, err
		}
		restore := []factState{}
		for rows.Next() {
			f, e := scanFactState(rows)
			if e != nil {
				rows.Close()
				return result, e
			}
			restore = append(restore, f)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return result, err
		}
		if len(restore) > 64 {
			return result, errors.New("memory: review undo exceeds bound")
		}
		for _, prior := range restore {
			current, err := s.loadReviewFact(ctx, prior.ID)
			if err != nil {
				return result, err
			}
			if current.Commit != reverseCommit {
				return result, errFactReviewConflict
			}
			restored, err := scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state=$2,superseded_at=$3,invalidated_at=$4,suppressed=$5,confidence=$6,authority_rank=$7,version=$8,commit_id=$9
 WHERE id=$1 AND commit_id=$10 RETURNING `+factStateColumns, prior.ID, prior.Lifecycle, prior.Superseded, prior.Invalidated, prior.Suppressed, prior.Confidence, prior.Rank, prior.Version, commit, reverseCommit))
			if store.IsNoRows(err) {
				return result, errFactReviewConflict
			}
			if err != nil {
				return result, err
			}
			if err = s.recordFactChange(ctx, commit, "undo", "restore approved incumbent", current.factState, restored); err != nil {
				return result, err
			}
		}
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO fact_review_actions(assertion_id,action,prior_lifecycle,new_lifecycle,actor_principal,commit_id,reverses_review_id)
 VALUES($1,$2,$3,$4,$5,$6,$7)`, id, action, before.Lifecycle, next, actor.Principal, commit, reverseID); err != nil {
		return result, err
	}
	if err = s.closeFactCommit(ctx, commit, id); err != nil {
		return result, err
	}
	return factMutationResult{AssertionID: id, CommitID: commit, Lifecycle: next, Changed: true}, nil
}

func (s *postgresDataStore) factCandidates(ctx context.Context, limit int) ([]map[string]any, error) {
	if limit < 1 || limit > 64 {
		limit = 64
	}
	rows, err := s.db.Query(ctx, `SELECT e.id,e.source,e.relation,e.target,e.assertion_kind,e.lifecycle_state,e.authority_rank,
 (SELECT count(*) FROM fact_evidence f WHERE f.assertion_id=e.id AND f.stance='supports' AND f.invalidated_at=''
 AND (f.source_kind<>'memory' OR EXISTS(SELECT 1 FROM memories m WHERE f.source_id='memory:'||m.id::text AND m.lifecycle_state='active' AND m.activation_suppressed=0))),e.commit_id
 FROM entity_edges e WHERE e.edge_class='semantic' AND e.lifecycle_state='candidate' AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0
 AND `+factEvidenceVisible+` ORDER BY e.id LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []map[string]any{}
	for rows.Next() {
		var id, count int64
		var rank int
		var subject, relation, object, kind, lifecycle, commit string
		if err = rows.Scan(&id, &subject, &relation, &object, &kind, &lifecycle, &rank, &count, &commit); err != nil {
			return nil, err
		}
		out = append(out, map[string]any{"id": id, "subject": subject, "relation": relation, "object": object, "assertion_kind": kind, "lifecycle": lifecycle, "authority_rank": rank, "evidence_count": count, "commit_id": commit})
	}
	return out, rows.Err()
}
