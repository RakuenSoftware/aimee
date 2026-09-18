package memory

import (
	"context"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

var errFactAnnotateOnly = errors.New("memory: historical evidence may only be annotated")
var errFactOperatorOnly = errors.New("memory: fact requires operator authority")

// invalidateFacts makes an explicit correction, retaining both history and a
// resurrection tombstone. It shares the assertion/review transaction lock.
func (s *postgresDataStore) invalidateFacts(ctx context.Context, actor FactActor, source, relation, target string) (int, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !validMutationActor(actor) || source == "" || relation == "" {
		return 0, errors.New("memory: fact invalidation requires an actor transaction")
	}
	if relation == "born_in" && actor.Rank != 40 {
		return 0, errFactOperatorOnly
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return 0, err
	}
	// Select only visible assertions. Hidden memory evidence must not become a
	// writable fact merely because its subject/relation can be guessed.
	rows, err := s.db.Query(ctx, `SELECT `+factStateColumns+`,source,relation,target,commit_id,epistemic_kind FROM entity_edges e
 WHERE edge_class='semantic' AND source=$1 AND relation=$2 AND ($3='' OR target=$3)
 AND superseded_at='' AND invalidated_at='' AND suppressed=0 AND `+factEvidenceVisible+`
 ORDER BY id LIMIT 65 FOR UPDATE`, source, relation, target)
	if err != nil {
		return 0, err
	}
	facts := []reviewFact{}
	for rows.Next() {
		var f reviewFact
		if err = rows.Scan(&f.ID, &f.Lifecycle, &f.Superseded, &f.Invalidated, &f.Suppressed, &f.Confidence, &f.Rank, &f.Version, &f.Source, &f.Relation, &f.Target, &f.Commit, &f.Kind); err != nil {
			rows.Close()
			return 0, err
		}
		facts = append(facts, f)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	if len(facts) > 64 {
		return 0, errors.New("memory: fact invalidation exceeds bound")
	}
	writable := []reviewFact{}
	for _, f := range facts {
		if f.Kind == "episode" || f.Kind == "experience" {
			return 0, errFactAnnotateOnly
		}
		if f.Kind == "policy" && actor.Rank != 40 {
			return 0, errFactOperatorOnly
		}
		if f.Rank <= actor.Rank {
			writable = append(writable, f)
		}
	}
	if len(writable) == 0 {
		return 0, nil
	}
	commit, err := s.openFactCommit(ctx, actor, "fact.invalidate", "")
	if err != nil {
		return 0, err
	}
	var lastID int64
	for _, before := range writable {
		after, err := scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state='invalidated',invalidated_at=pg_now_text(),version=version+1,commit_id=$2 WHERE id=$1 RETURNING `+factStateColumns, before.ID, commit))
		if err != nil {
			return 0, err
		}
		if err = s.recordFactChange(ctx, commit, "invalidate", "reversible correction", before.factState, after); err != nil {
			return 0, err
		}
		if _, err = s.db.Exec(ctx, `INSERT INTO memory_rejection_tombstones(object_kind,source,relation,target,authority_rank,reason,rejected_by)
 VALUES('fact',$1,$2,$3,$4,'explicit fact invalidation',$5) ON CONFLICT DO NOTHING`, before.Source, before.Relation, before.Target, actor.Rank, actor.Principal); err != nil {
			return 0, err
		}
		lastID = before.ID
	}
	if err = s.closeFactCommit(ctx, commit, lastID); err != nil {
		return 0, err
	}
	return len(writable), nil
}

func (s *postgresDataStore) retractContextQuery(ctx context.Context, query string) (string, error) {
	if !IsRetraction(query) {
		return "", nil
	}
	attr, ok := PossessiveAttr(query)
	if !ok {
		return "", nil
	}
	// The query is model-composed, including when a verified user's session
	// transports it. Neither JSON authority nor authenticated identity raises it.
	count, err := s.invalidateFacts(ctx, modelFactActor(), "user", attr, "")
	if errors.Is(err, errFactAnnotateOnly) {
		return "annotate_only", nil
	}
	if errors.Is(err, errFactOperatorOnly) {
		return "operator_required", nil
	}
	if err != nil {
		return "", err
	}
	if count == 0 {
		return "unchanged", nil
	}
	return "invalidated", nil
}
