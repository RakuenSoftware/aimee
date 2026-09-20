package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

// Candidate maintenance is a bounded system transition, never user/operator
// authority. It shares the mutation lock, change log and WORM seal with writes.
func (s *postgresDataStore) maintainFacts(ctx context.Context, action string, value int) (int, error) {
	if s.placement != PlacementKB || value <= 0 || value > 36500 || (action != "promote" && action != "expire") {
		return 0, errors.New("memory: invalid fact maintenance")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return 0, errors.New("memory: fact maintenance requires transaction")
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return 0, err
	}
	condition := `e.confidence_class='B' AND (SELECT count(*) FROM fact_evidence f WHERE f.assertion_id=e.id AND f.stance='supports' AND f.invalidated_at='') >= $1`
	params := []any{value}
	if action == "expire" {
		condition = `e.confidence_class='C' AND e.asserted_at<>'' AND replace(substr(e.asserted_at,1,19),'T',' ') < to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC' - $1::int * interval '1 day','YYYY-MM-DD HH24:MI:SS') AND (SELECT count(*) FROM fact_evidence f WHERE f.assertion_id=e.id AND f.stance='supports' AND f.invalidated_at='')<=1`
	} else {
		functional, _ := json.Marshal(functionalFactRelations)
		params = append(params, string(functional))
		// Filter keyed conflicts before the batch; legacy Unicode identities receive
		// the canonical writer's check below, followed by the next keyset page.
		condition += ` AND (e.relation NOT IN (SELECT jsonb_array_elements_text($2::jsonb)) OR NOT EXISTS(SELECT 1 FROM entity_edges incumbent WHERE incumbent.id<>e.id AND incumbent.edge_class='semantic' AND incumbent.lifecycle_state IN ('persistent','promoted') AND incumbent.superseded_at='' AND incumbent.invalidated_at='' AND incumbent.suppressed=0 AND ((incumbent.identity_subject_key<>'' AND incumbent.identity_subject_key=e.identity_subject_key) OR (incumbent.source=e.source AND incumbent.relation=e.relation))))`
	}
	params = append(params, int64(0))
	query := `SELECT ` + factStateColumns + `,source,relation,target FROM entity_edges e WHERE e.edge_class='semantic' AND e.lifecycle_state='candidate' AND e.authority_rank<=20 AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0 AND ` + condition + fmt.Sprintf(` AND e.id>$%d ORDER BY e.id LIMIT 64 FOR UPDATE`, len(params))
	type entry struct {
		state                    factState
		source, relation, target string
	}
	commit := ""
	changed := 0
	for changed < 64 {
		rows, err := s.db.Query(ctx, query, params...)
		if err != nil {
			return 0, err
		}
		candidates := []entry{}
		for rows.Next() {
			var e entry
			f := &e.state
			if err = rows.Scan(&f.ID, &f.Lifecycle, &f.Superseded, &f.Invalidated, &f.Suppressed, &f.Confidence, &f.Rank, &f.Version, &e.source, &e.relation, &e.target); err != nil {
				break
			}
			candidates = append(candidates, e)
		}
		if err == nil {
			err = rows.Err()
		}
		rows.Close()
		if err != nil {
			return 0, err
		}
		for _, entry := range candidates {
			before := entry.state
			params[len(params)-1] = before.ID
			if action == "promote" {
				in := factAssertion{FactCandidate: FactCandidate{Subject: entry.source, Relation: entry.relation, Object: entry.target}}
				identity, subject := factIdentity(entry.source, entry.relation, entry.target)
				rejected, err := s.factTombstoned(ctx, in, identity)
				if err != nil {
					return 0, err
				}
				if rejected {
					continue
				}
				if factFunctional(entry.relation) {
					// Corroboration cannot replace the operator review needed to resolve a
					// quarantined contradiction, including unkeyed Unicode legacy rows.
					var incumbent bool
					err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM entity_edges WHERE id<>$1 AND edge_class='semantic' AND lifecycle_state IN ('persistent','promoted') AND superseded_at='' AND invalidated_at='' AND suppressed=0 AND ((identity_subject_key<>'' AND identity_subject_key=$2) OR (source=$3 AND relation=$4)))`, before.ID, subject, entry.source, entry.relation).Scan(&incumbent)
					if err != nil {
						return 0, err
					}
					if !incumbent {
						_, priors, err := s.legacyFactMatches(ctx, in, identity, subject)
						if err != nil {
							return 0, err
						}
						for _, prior := range priors {
							if prior.Lifecycle == "persistent" || prior.Lifecycle == "promoted" {
								incumbent = true
								break
							}
						}
					}
					if incumbent {
						continue
					}
				}
			}
			if commit == "" {
				actor := FactActor{Principal: "system:kb-maintenance", TransportIdentity: "internal", Role: "system", Rank: 20}
				commit, err = s.openFactCommit(ctx, actor, "fact.maintenance."+action, "")
				if err != nil {
					return 0, err
				}
			}
			update := `UPDATE entity_edges SET lifecycle_state='persistent',confidence=greatest(confidence,0.8),version=version+1,commit_id=$2 WHERE id=$1 RETURNING ` + factStateColumns
			if action == "expire" {
				update = `UPDATE entity_edges SET lifecycle_state='invalidated',invalidated_at=pg_now_text(),version=version+1,commit_id=$2 WHERE id=$1 RETURNING ` + factStateColumns
			}
			after, err := scanFactState(s.db.QueryRow(ctx, update, before.ID, commit))
			if err != nil {
				return 0, err
			}
			if err = s.recordFactChange(ctx, commit, action, "candidate maintenance", before, after); err != nil {
				return 0, err
			}
			changed++
			if changed == 64 {
				break
			}
		}
		if len(candidates) < 64 {
			break
		}
	}
	if commit != "" {
		if err := s.closeFactCommit(ctx, commit, 0); err != nil {
			return 0, err
		}
	}
	return changed, nil
}
