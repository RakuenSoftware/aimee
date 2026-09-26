package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
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
	table, fields, scopeClause, lifecycle := "user_memories", "id,tier,kind,key,content,confidence", "", "(valid_until IS NULL OR valid_until > now())"
	idsJSON, _ := json.Marshal(restriction.IDs)
	args := []any{string(idsJSON), query, kind, tier, limit}
	if s.placement == PlacementKB {
		table = "memories"
		fields = "id,scope_type,scope_value,tier,kind,key,content,confidence"
		scopeClause = " AND scope_type=$6 AND scope_value=$7"
		lifecycle = "activation_suppressed=0 AND (COALESCE(valid_from,'')='' OR aimee_utc_text_timestamptz(valid_from)<=now()) AND (COALESCE(valid_until,'')='' OR aimee_utc_text_timestamptz(valid_until)>now())"
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
WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint) AND lifecycle_state='active' AND %s%s
 AND ($3='' OR kind=$3) AND ($4='' OR tier=$4)
 AND document.v @@ q.terms
 AND (cardinality(q.lexemes)=1
  OR (SELECT count(*) FROM unnest(tsvector_to_array(document.v)) AS matched(term) WHERE term=ANY(q.lexemes))>=2
  OR EXISTS (SELECT 1 FROM unnest(q.lexemes) AS matched(term) WHERE term ~ '[-_./]' AND term=ANY(tsvector_to_array(document.v))))
ORDER BY ts_rank_cd(document.v,q.terms) DESC,
 confidence DESC,updated_at DESC,id DESC LIMIT $5`, vector("$2::text"), fields, table, vector("key||' '||content"), lifecycle, scopeClause)
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
