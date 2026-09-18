package memory

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"math"
	"sort"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

var errFactTombstoned = errors.New("memory: fact is explicitly rejected")

type factAssertion struct {
	FactCandidate
	RelationID      int64
	ConfidenceClass string
	Confidence      float64
	Functional      bool
}

type factMutationResult struct {
	AssertionID   int64  `json:"assertion_id"`
	CommitID      string `json:"commit_id,omitempty"`
	Lifecycle     string `json:"lifecycle"`
	Changed       bool   `json:"changed"`
	Quarantined   bool   `json:"quarantined"`
	EvidenceAdded bool   `json:"evidence_added"`
}

type factState struct {
	ID                                 int64
	Lifecycle, Superseded, Invalidated string
	Suppressed                         int
	Confidence                         float64
	Rank, Version                      int
}

const factStateColumns = `id,lifecycle_state,superseded_at,invalidated_at,suppressed,confidence,authority_rank,version`

func scanFactState(row store.Row) (factState, error) {
	var f factState
	err := row.Scan(&f.ID, &f.Lifecycle, &f.Superseded, &f.Invalidated, &f.Suppressed, &f.Confidence, &f.Rank, &f.Version)
	return f, err
}

func validMutationActor(a FactActor) bool {
	if a.Principal == "" || len(a.Principal) > 1024 || a.TransportIdentity == "" {
		return false
	}
	switch a.Rank {
	case 10:
		return a.Role == "model"
	case 20:
		return a.Role == "system"
	case 30:
		return a.Role == "user"
	case 40:
		return a.Role == "operator" && a.Authenticated == 1
	}
	return false
}

func (s *postgresDataStore) openFactCommit(ctx context.Context, a FactActor, operation, parent string) (string, error) {
	if !validMutationActor(a) {
		return "", errors.New("memory: invalid fact actor")
	}
	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return "", err
	}
	h := hex.EncodeToString(nonce[:])
	id := h[:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:]
	if _, err := s.db.Exec(ctx, `SELECT set_config('aimee.principal',$1,true),set_config('aimee.authority',$2,true),set_config('aimee.transport_identity',$3,true),set_config('aimee.correlation_id',$4,true)`, a.Principal, a.Role, a.TransportIdentity, parent); err != nil {
		return "", err
	}
	_, err := s.db.Exec(ctx, `INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible,parent_commit_id,origin_ref) VALUES($1,$2,$3,$4,$5,'open',1,$6,$6)`, id, operation, a.Principal, a.Role, a.Rank, parent)
	return id, err
}

func (s *postgresDataStore) closeFactCommit(ctx context.Context, commit string, id int64) error {
	tag, err := s.db.Exec(ctx, `UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id=$1 AND status='open'`, commit)
	if err != nil {
		return err
	}
	if tag.RowsAffected() != 1 {
		return errors.New("memory: fact commit is not open")
	}
	_, err = s.db.Exec(ctx, `SELECT kb_fact_commit_worm_seal($1,$2)`, commit, strconv.FormatInt(id, 10))
	return err
}

func (s *postgresDataStore) recordFactChange(ctx context.Context, commit, action, detail string, before, after factState) error {
	id := after.ID
	if id == 0 {
		id = before.ID
	}
	exists := func(id int64) int {
		if id > 0 {
			return 1
		}
		return 0
	}
	_, err := s.db.Exec(ctx, `INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action,
 existed_before,existed_after,before_lifecycle,after_lifecycle,before_superseded_at,after_superseded_at,
 before_invalidated_at,after_invalidated_at,before_suppressed,after_suppressed,before_confidence,after_confidence,
 before_authority_rank,after_authority_rank,before_version,after_version,diff_detail)
 VALUES($1,$2,'assertion',$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21)`, commit, id, strconv.FormatInt(id, 10), action,
		exists(before.ID), exists(after.ID), before.Lifecycle, after.Lifecycle, before.Superseded, after.Superseded, before.Invalidated, after.Invalidated, before.Suppressed, after.Suppressed, before.Confidence, after.Confidence, before.Rank, after.Rank, before.Version, after.Version, detail)
	return err
}

// Resolve pre-migration identities without rewriting unrelated historical
// assertions. Matching rows acquire keys only inside their audited mutation.
func (s *postgresDataStore) legacyFactMatches(ctx context.Context, in factAssertion, identity, subject string) (factState, []factState, error) {
	var exact factState
	priors := []factState{}
	afterID := int64(0)
	for {
		rows, err := s.db.Query(ctx, `SELECT `+factStateColumns+`,source,relation,target FROM entity_edges
 WHERE edge_class='semantic' AND (identity_key='' OR identity_subject_key='') AND id>$1 ORDER BY id LIMIT 256 FOR UPDATE`, afterID)
		if err != nil {
			return exact, nil, err
		}
		count := 0
		for rows.Next() {
			var f factState
			var source, relation, target string
			if err = rows.Scan(&f.ID, &f.Lifecycle, &f.Superseded, &f.Invalidated, &f.Suppressed, &f.Confidence, &f.Rank, &f.Version, &source, &relation, &target); err != nil {
				rows.Close()
				return exact, nil, err
			}
			count++
			afterID = f.ID
			key, sk := factIdentity(source, relation, target)
			if (identity != "" && key == identity) || (source == in.Subject && relation == in.Relation && target == in.Object) {
				exact = f
				continue
			}
			if (in.Functional || factFunctional(in.Relation)) && ((subject != "" && sk == subject) || (source == in.Subject && relation == in.Relation)) && f.Superseded == "" && f.Invalidated == "" && f.Suppressed == 0 && (f.Lifecycle == "candidate" || f.Lifecycle == "persistent" || f.Lifecycle == "promoted") {
				priors = append(priors, f)
				if len(priors) > 64 {
					rows.Close()
					return exact, nil, errors.New("memory: legacy functional correction exceeds bound")
				}
			}
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return exact, nil, err
		}
		if count < 256 {
			break
		}
	}
	return exact, priors, nil
}

// Rejection survives alternate Unicode/case spellings as well as exact replay.
func (s *postgresDataStore) factTombstoned(ctx context.Context, in factAssertion, identity string) (bool, error) {
	afterID := int64(0)
	for {
		rows, err := s.db.Query(ctx, `SELECT id,source,relation,target FROM memory_rejection_tombstones WHERE object_kind='fact' AND active=1 AND id>$1 ORDER BY id LIMIT 256`, afterID)
		if err != nil {
			return false, err
		}
		count := 0
		for rows.Next() {
			var source, relation, target string
			if err = rows.Scan(&afterID, &source, &relation, &target); err != nil {
				rows.Close()
				return false, err
			}
			count++
			key, _ := factIdentity(source, relation, target)
			if (identity != "" && key == identity) || (source == in.Subject && relation == in.Relation && target == in.Object) {
				rows.Close()
				return true, nil
			}
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return false, err
		}
		if count < 256 {
			return false, nil
		}
	}
}

// assertFact runs inside the memory owner's transaction. Its caller must roll
// back on any error, including a failed audit seal. Public input never supplies
// the actor: workers load captured authority and operators use verified context.
func (s *postgresDataStore) assertFact(ctx context.Context, in factAssertion) (factMutationResult, error) {
	var result factMutationResult
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB {
		return result, errors.New("memory: fact mutation requires a KB transaction")
	}
	if !validMutationActor(in.Actor) || in.Subject == "" || in.Relation == "" || in.Object == "" || len(in.Subject) > 4096 || len(in.Object) > 4096 || math.IsNaN(in.Confidence) || math.IsInf(in.Confidence, 0) {
		return result, errors.New("memory: invalid assertion")
	}
	switch in.AssertionKind {
	case "":
		in.AssertionKind = "world_fact"
	case "world_fact", "episode", "experience", "mental_model", "preference", "instruction", "policy", "hypothesis":
	default:
		return result, errors.New("memory: invalid assertion kind")
	}
	if in.ConfidenceClass == "" {
		in.ConfidenceClass = "C"
	}
	if in.ConfidenceClass != "A" && in.ConfidenceClass != "B" && in.ConfidenceClass != "C" {
		return result, errors.New("memory: invalid confidence class")
	}
	in.Confidence = math.Max(0, math.Min(1, in.Confidence))
	// Serialize assertions and functional corrections. This
	// also closes the old read-then-insert race for absent normalized identities.
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return result, err
	}
	identity, subject := factIdentity(in.Subject, in.Relation, in.Object)
	rejected, err := s.factTombstoned(ctx, in, identity)
	if err != nil {
		return result, err
	}
	if rejected {
		return result, errFactTombstoned
	}
	exact, err := scanFactState(s.db.QueryRow(ctx, `SELECT `+factStateColumns+` FROM entity_edges WHERE edge_class='semantic' AND
 (($4<>'' AND identity_key=$4) OR ((identity_key='' OR $4='') AND source=$1 AND relation=$2 AND target=$3)) ORDER BY id DESC LIMIT 1 FOR UPDATE`, in.Subject, in.Relation, in.Object, identity))
	if err != nil && !store.IsNoRows(err) {
		return result, err
	}
	legacyExact, legacyPriors, err := s.legacyFactMatches(ctx, in, identity, subject)
	if err != nil {
		return result, err
	}
	if legacyExact.ID > exact.ID {
		exact = legacyExact
	}
	e := in.Evidence
	if e.SourceKind == "" {
		e.SourceKind = "observation"
	}
	if e.Stance != "contradicts" {
		e.Stance = "supports"
	}
	if exact.ID > 0 && e.SourceID != "" {
		var replay bool
		if err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM fact_evidence WHERE assertion_id=$1 AND source_kind=$2 AND source_id=$3 AND source_span=$4 AND evidence_hash=$5 AND stance=$6)`, exact.ID, e.SourceKind, e.SourceID, e.SourceSpan, e.EvidenceHash, e.Stance).Scan(&replay); err != nil {
			return result, err
		}
		if replay {
			return factMutationResult{AssertionID: exact.ID, Lifecycle: exact.Lifecycle}, nil
		}
	}
	commit, err := s.openFactCommit(ctx, in.Actor, "fact.assert", e.IngestRunID)
	if err != nil {
		return result, err
	}
	desired := "candidate"
	if in.Actor.Rank >= 20 {
		desired = "persistent"
	}
	reactivate := exact.ID > 0 && (exact.Lifecycle == "invalidated" || exact.Lifecycle == "superseded") && in.Actor.Rank >= exact.Rank
	promote := exact.Lifecycle == "candidate" && in.Actor.Rank >= 20 && in.Actor.Rank >= exact.Rank
	priorID := int64(0)
	if (in.Functional || factFunctional(in.Relation)) && (exact.ID == 0 || reactivate || promote) {
		rows, err := s.db.Query(ctx, `SELECT `+factStateColumns+` FROM entity_edges WHERE edge_class='semantic'
 AND ((identity_subject_key<>'' AND identity_subject_key=$1) OR (identity_subject_key='' AND source=$2 AND relation=$3))
 AND id<>$4 AND superseded_at='' AND invalidated_at='' AND suppressed=0
 AND (lifecycle_state IN ('persistent','promoted') OR ($4=0 AND lifecycle_state='candidate'))
 ORDER BY authority_rank DESC,id DESC LIMIT 65 FOR UPDATE`, subject, in.Subject, in.Relation, exact.ID)
		if err != nil {
			return result, err
		}
		priors := []factState{}
		for rows.Next() {
			f, scanErr := scanFactState(rows)
			if scanErr != nil {
				rows.Close()
				return result, scanErr
			}
			priors = append(priors, f)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return result, err
		}
		if len(priors) > 64 {
			return result, errors.New("memory: functional correction exceeds bound")
		}

		seen := map[int64]bool{}
		for _, p := range priors {
			seen[p.ID] = true
		}
		for _, p := range legacyPriors {
			if p.ID != exact.ID && !seen[p.ID] && (exact.ID == 0 || p.Lifecycle != "candidate") {
				priors = append(priors, p)
				seen[p.ID] = true
			}
		}
		if len(priors) > 64 {
			return result, errors.New("memory: functional correction exceeds bound")
		}
		sort.Slice(priors, func(i, j int) bool {
			if priors[i].Rank != priors[j].Rank {
				return priors[i].Rank > priors[j].Rank
			}
			return priors[i].ID > priors[j].ID
		})
		for _, prior := range priors {
			if in.Actor.Rank < prior.Rank || (in.Relation == "born_in" && in.Actor.Rank != 40) {
				result.Quarantined = true
			}
		}
		if result.Quarantined {
			desired = "candidate"
			reactivate = false
			promote = false
		} else {
			for _, prior := range priors {
				after, err := scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state='superseded',superseded_at=pg_now_text(),version=version+1,commit_id=$2 WHERE id=$1 RETURNING `+factStateColumns, prior.ID, commit))
				if err != nil {
					return result, err
				}
				if err = s.recordFactChange(ctx, commit, "supersede", "functional relation correction", prior, after); err != nil {
					return result, err
				}
				if priorID == 0 {
					priorID = prior.ID
				}
			}
		}
	}
	var after factState
	if exact.ID > 0 {
		next := exact.Lifecycle
		if reactivate || promote || result.Quarantined {
			next = desired
		}
		rank := max(exact.Rank, in.Actor.Rank)
		confidence := math.Max(exact.Confidence, in.Confidence)
		result.Changed = reactivate || next != exact.Lifecycle || rank != exact.Rank || confidence != exact.Confidence
		changed := 0
		if result.Changed {
			changed = 1
		}
		after, err = scanFactState(s.db.QueryRow(ctx, `UPDATE entity_edges SET lifecycle_state=$2,
 superseded_at=CASE WHEN $3 THEN '' ELSE superseded_at END,invalidated_at=CASE WHEN $3 THEN '' ELSE invalidated_at END,
 suppressed=CASE WHEN $3 THEN 0 ELSE suppressed END,confidence=$4,
 confidence_class=CASE WHEN $5>authority_rank THEN $6 ELSE confidence_class END,authority_rank=$7,
 actor_principal=CASE WHEN $5>authority_rank THEN $8 ELSE actor_principal END,version=version+$9,commit_id=$10,
 identity_key=CASE WHEN identity_key='' THEN $11 ELSE identity_key END,
 identity_subject_key=CASE WHEN identity_subject_key='' THEN $12 ELSE identity_subject_key END
 WHERE id=$1 RETURNING `+factStateColumns, exact.ID, next, reactivate, confidence, in.Actor.Rank, in.ConfidenceClass, rank, in.Actor.Principal, changed, commit, identity, subject))
		if err != nil {
			return result, err
		}
		err = s.recordFactChange(ctx, commit, "corroborate", "independent evidence mention", exact, after)
	} else {
		after, err = scanFactState(s.db.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target,weight,relation_id,subject_kind,object_kind,
 edge_class,confidence_class,confidence,asserted_at,valid_from,valid_until,assertion_kind,epistemic_kind,lifecycle_state,authority_rank,
 actor_principal,version,prior_version_id,commit_id,identity_key,identity_subject_key)
 VALUES($1,$2,$3,1,$4,$5,$6,'semantic',$7,$8,pg_now_text(),$9,$10,$11,$11,$12,$13,$14,1,$15,$16,$17,$18) RETURNING `+factStateColumns,
			in.Subject, in.Relation, in.Object, in.RelationID, int(in.SubjectKind), int(in.ObjectKind), in.ConfidenceClass, in.Confidence, in.ValidFrom, in.ValidUntil, in.AssertionKind, desired, in.Actor.Rank, in.Actor.Principal, priorID, commit, identity, subject))
		if err != nil {
			return result, err
		}
		result.Changed = true
		detail := "new assertion"
		if result.Quarantined {
			detail = "quarantined below incumbent authority"
		}
		err = s.recordFactChange(ctx, commit, "insert", detail, factState{}, after)
	}
	if err != nil {
		return result, err
	}
	if e.SourceID == "" {
		e.SourceID = commit
	}
	if e.ActorPrincipal == "" {
		e.ActorPrincipal = in.Actor.Principal
	}
	tag, err := s.db.Exec(ctx, `INSERT INTO fact_evidence(assertion_id,source_kind,source_id,source_span,evidence_hash,actor_principal,observed_at,ingest_run_id,commit_id,stance)
 VALUES($1,$2,$3,$4,$5,$6,COALESCE(NULLIF($7,''),pg_now_text()),$8,$9,$10) ON CONFLICT DO NOTHING`, after.ID, e.SourceKind, e.SourceID, e.SourceSpan, e.EvidenceHash, e.ActorPrincipal, e.ObservedAt, e.IngestRunID, commit, e.Stance)
	if err != nil {
		return result, err
	}
	result.EvidenceAdded = tag.RowsAffected() > 0
	if err = s.closeFactCommit(ctx, commit, after.ID); err != nil {
		return result, err
	}
	result.AssertionID, result.CommitID, result.Lifecycle = after.ID, commit, after.Lifecycle
	return result, nil
}
