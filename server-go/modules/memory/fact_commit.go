package memory

import (
	"context"
	"errors"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

func entityAliasName(name string) string {
	var b strings.Builder
	pending := false
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' {
			pending = b.Len() > 0
			continue
		}
		if pending {
			b.WriteByte(' ')
			pending = false
		}
		b.WriteByte(toLower(c))
	}
	return b.String()
}

func (s *postgresDataStore) canonicalFactEndpoint(ctx context.Context, name string, kind NodeKind) (string, error) {
	switch kind {
	case NodePerson, NodePlace, NodeDevice, NodeOrg, NodeIp:
	default:
		return name, nil
	}
	normalized := entityAliasName(name)
	if normalized == "" || len(normalized) > 255 {
		return "", errors.New("memory: invalid entity name")
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,7197))`, normalized); err != nil {
		return "", err
	}
	var id int64
	var suppressed int
	err := s.db.QueryRow(ctx, `SELECT CASE WHEN r.status='merged' AND r.merged_into>0 THEN r.merged_into ELSE r.canonical_id END,a.suppressed
 FROM entity_aliases a JOIN entity_registry r ON r.canonical_id=a.canonical_id WHERE a.name_norm=$1`, normalized).Scan(&id, &suppressed)
	if store.IsNoRows(err) {
		if err = s.db.QueryRow(ctx, `INSERT INTO entity_registry(kind,status) VALUES($1,'active') RETURNING canonical_id`, int(kind)).Scan(&id); err != nil {
			return "", err
		}
		if _, err = s.db.Exec(ctx, `INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES($1,$2,$3,1)`, name, normalized, id); err != nil {
			return "", err
		}
	} else if err != nil {
		return "", err
	} else if suppressed != 0 {
		return "", errors.New("memory: entity alias is suppressed")
	}
	var canonical string
	err = s.db.QueryRow(ctx, `SELECT name FROM entity_aliases WHERE canonical_id=$1 AND suppressed=0 ORDER BY is_preferred DESC,id LIMIT 1`, id).Scan(&canonical)
	if err != nil {
		return "", err
	}
	return canonical, nil
}

// commitFactCandidate owns the complete gate-to-commit path. Caller-owned
// transactions include endpoint registration, ontology staging, assertion,
// evidence, history and the durable WORM intent.
func (s *postgresDataStore) commitFactCandidate(ctx context.Context, candidate FactCandidate) (factMutationResult, FactVerdict, error) {
	var empty factMutationResult
	decision := DecideFactWrite(FactWriteRequest{Head: candidate.SubjectKind, Relation: candidate.Relation, Tail: candidate.ObjectKind})
	if !decision.CommitAllowed {
		return empty, decision.Verdict, errors.New("memory: fact gate withheld candidate")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return empty, decision.Verdict, errors.New("memory: fact commit requires transaction")
	}
	var err error
	// Gate both endpoints before they can become registry aliases or graph text.
	candidate.Subject, err = screenMemoryText(candidate.Subject)
	if err != nil {
		return empty, decision.Verdict, err
	}
	candidate.Object, err = screenMemoryText(candidate.Object)
	if err != nil {
		return empty, decision.Verdict, err
	}
	candidate.Relation = normalizeRelType(candidate.Relation)
	// Acquire the shared mutation lock before per-name locks to keep opposite
	// endpoint orders from deadlocking concurrent functional corrections.
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return empty, decision.Verdict, err
	}
	candidate.Subject, err = s.canonicalFactEndpoint(ctx, candidate.Subject, candidate.SubjectKind)
	if err != nil {
		return empty, decision.Verdict, err
	}
	candidate.Object, err = s.canonicalFactEndpoint(ctx, candidate.Object, candidate.ObjectKind)
	if err != nil {
		return empty, decision.Verdict, err
	}
	var id int64
	var status string
	err = s.db.QueryRow(ctx, `SELECT id,status FROM rel_types WHERE rel_type=$1`, candidate.Relation).Scan(&id, &status)
	if store.IsNoRows(err) && decision.Verdict == FactNovel {
		err = s.db.QueryRow(ctx, `INSERT INTO rel_types(rel_type,status,sensitivity) VALUES($1,'provisional','pii') ON CONFLICT(rel_type) DO UPDATE SET rel_type=EXCLUDED.rel_type RETURNING id,status`, candidate.Relation).Scan(&id, &status)
	}
	if err != nil {
		return empty, decision.Verdict, err
	}
	known := decision.Verdict == FactAccept || status == "active"
	class, confidence := "C", .4
	if known {
		class, confidence = "B", .6
		if candidate.Actor.Rank >= 30 {
			class, confidence = "A", 1
		}
	}
	result, err := s.assertFact(ctx, factAssertion{FactCandidate: candidate, RelationID: id, ConfidenceClass: class, Confidence: confidence})
	if err != nil {
		return result, decision.Verdict, err
	}
	if known {
		return result, FactAccept, nil
	}
	// Delivery replay must not count twice toward ontology promotion.
	if result.EvidenceAdded {
		_, err = s.db.Exec(ctx, `INSERT INTO ontology_evaluations(rel_type,occurrence_count,status,created_at) VALUES($1,1,'pending',pg_now_text()) ON CONFLICT(rel_type) DO UPDATE SET occurrence_count=ontology_evaluations.occurrence_count+1`, candidate.Relation)
	}
	return result, FactNovel, err
}
