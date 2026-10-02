package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/memory/readcontract"
)

type SearchRestriction = readcontract.SearchRestriction

type restrictedSearchStore interface {
	SearchRestricted(context.Context, Scope, string, string, string, int, SearchRestriction) ([]Record, error)
}

// This versioned lexical operation accepts noisy keyword clusters using OR
// full-text matching. Ordinary Search retains its existing phrase contract.
// Multi-term clusters need two shared lexemes or one exact compound address;
// a lone generic word such as "city" is not enough evidence for a noisy query.
// Candidate authorization, lifecycle and scope are applied before rank/LIMIT.
func (s *postgresDataStore) SearchRestricted(ctx context.Context, scope Scope, query, kind, tier string, limit int, restriction SearchRestriction) ([]Record, error) {
	if err := restriction.Validate(); err != nil {
		return nil, err
	}
	if limit < 1 || limit > 100 || strings.TrimSpace(query) == "" {
		return nil, errors.New("memory: invalid restricted search")
	}
	if len(restriction.IDs) == 0 {
		return []Record{}, nil
	}
	table, fields, scopeClause, lifecycle := "user_memories m", "m.id,m.tier,m.kind,m.key,m.content,m.confidence", "", personalCurrentMemorySQL("m.")
	idsJSON, _ := json.Marshal(restriction.IDs)
	args := []any{string(idsJSON), query, kind, tier, limit}
	if s.placement == PlacementKB {
		table = "memories m"
		fields = "m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence"
		scopeClause = " AND m.scope_type=$6 AND m.scope_value=$7"
		lifecycle = currentMemorySQL("m.")
		args = append(args, scope.Type, scope.Value)
	}
	// Preserve compound entity identifiers as one lexeme. PostgreSQL's default
	// parser also indexes their fragments ("unit-abc" -> "unit"), which makes
	// unrelated queries about measurement units retrieve personal entities.
	vector := func(text string) string {
		return fmt.Sprintf(`(SELECT COALESCE(string_agg(quote_literal(lexeme)||':'||least(position,16383)::text,' '),'')::tsvector
 FROM regexp_matches(lower(%s),'([[:alnum:]]+([-_./][[:alnum:]]+)*)','g') WITH ORDINALITY AS tokens(parts,position)
 CROSS JOIN LATERAL unnest(CASE WHEN parts[1] ~ '[-_./]' THEN ARRAY[parts[1]] ELSE tsvector_to_array(to_tsvector('english',parts[1])) END) AS words(lexeme))`, text)
	}
	sql := fmt.Sprintf(`WITH query_vector AS (SELECT %s AS v),
 q AS (SELECT COALESCE((SELECT string_agg(quote_literal(term),' | ') FROM unnest(tsvector_to_array(v)) AS terms(term)),'')::tsquery AS terms, tsvector_to_array(v) AS lexemes FROM query_vector)
SELECT %s FROM %s,q CROSS JOIN LATERAL (SELECT %s AS v) AS document
WHERE m.id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint) AND %s%s
 AND ($3='' OR m.kind=$3) AND ($4='' OR m.tier=$4)
 AND document.v @@ q.terms
 AND (cardinality(q.lexemes)=1
  OR (SELECT count(*) FROM unnest(tsvector_to_array(document.v)) AS matched(term) WHERE term=ANY(q.lexemes))>=2
  OR EXISTS (SELECT 1 FROM unnest(q.lexemes) AS matched(term) WHERE term ~ '[-_./]' AND term=ANY(tsvector_to_array(document.v))))
ORDER BY ts_rank_cd(document.v,q.terms) DESC,
 m.confidence DESC,m.updated_at DESC,m.id DESC LIMIT $5`, vector("$2::text"), fields, table, vector("m.key||' '||m.content"), lifecycle, scopeClause)
	rows, err := s.db.Query(ctx, sql, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRestrictedRecords(rows, s.placement, scope)
}
func scanRestrictedRecords(rows store.Rows, placement Placement, scope Scope) ([]Record, error) {
	records := []Record{}
	for rows.Next() {
		var r Record
		var err error
		if placement == PlacementServer {
			r.Scope = scope
			err = rows.Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
		} else {
			err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
		}
		if err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	return records, rows.Err()
}
