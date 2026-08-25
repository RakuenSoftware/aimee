package postgres

import (
	"context"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/db3"
)

// The in-database vector search: pgvector, or pgvectorscale's StreamingDiskANN
// where the extension is installed.
//
// THIS IS THE DEFAULT PATH, not a fallback in the sense of a degraded mode. A
// deployment that installs no external vector database serves every vector
// operation here, and that is the ordinary configuration. The routed path
// accelerates the portable subset when a provider is present; this answers
// everything, always, and is what makes the provider optional.
//
// The SQL mirrors what the C transport asks, because both read the same tables
// and must return the same rows. The distance operator is <=> (cosine), and the
// score is 1.0 - distance so that larger is nearer -- the same convention the
// wire uses, and the reason a provider returning a raw Euclid distance has to
// negate it.

// collectionTable maps a DB3 collection to the relation that holds it.
//
// A closed map rather than string interpolation: the collection arrives from a
// caller, and putting a caller-supplied name into SQL is how a search becomes an
// injection. An unknown collection is refused rather than guessed.
type vectorRelation struct {
	table  string
	vector string
}

var collectionTable = map[string]vectorRelation{
	"memory":            {"memory_embeddings", "embedding"},
	"kb":                {"kb_embeddings", "embedding"},
	"kb_pdf":            {"kb_pdf_embeddings", "embedding"},
	"code":              {"code_embeddings", "embedding"},
	"curator_entity":    {"curator_entity_vectors", "embedding"},
	"curator_narrative": {"curator_narrative_vectors", "embedding"},
}

// labelColumn maps a filter key to the column that holds it.
//
// Also closed, and for the same reason. A filter key is caller-supplied, and a
// key nobody expected must narrow nothing rather than widen everything: an
// unknown key makes the search refuse instead of quietly dropping the
// condition, because a dropped condition returns another workspace's rows.
var labelColumn = map[string]string{
	"project":     "project",
	"workspace":   "workspace",
	"record_type": "record_type",
	"kind":        "kind",
	"generation":  "generation",
	"scope_kind":  "scope_kind",
	"scope_id":    "scope_id",
	"status":      "status",
	"priority":    "priority",
}

// NewPGVectorSearch answers vector searches over one collection from PostgreSQL.
//
// The collection is bound HERE rather than read from the request, because the
// wire has no collection field: a provider serves exactly one, and record_type
// narrows within it rather than choosing it. The in-database side has to be told
// the same thing the provider was configured with, or the two would answer from
// different relations for the same request.
func NewPGVectorSearch(collection string) (PostgreSQLSearch, error) {
	relation, known := collectionTable[collection]
	if !known {
		return nil, fmt.Errorf("postgres: no relation for collection %q", collection)
	}
	return func(ctx context.Context, request db3.SearchRequest) (db3.SearchReply, error) {
		return pgVectorSearch(ctx, relation, request)
	}, nil
}

func pgVectorSearch(ctx context.Context, relation vectorRelation,
	request db3.SearchRequest) (db3.SearchReply, error) {
	if request.Validate() != nil {
		return db3.SearchReply{}, db3.ErrMalformed
	}

	pool, err := SQLPool(ctx)
	if err != nil || pool == nil {
		return db3.SearchReply{}, fmt.Errorf("postgres: no database for a vector search: %w", err)
	}

	// The scope is not advisory. Every condition the request carries becomes a
	// WHERE clause, and a scope that failed to narrow would return rows the
	// caller is not entitled to -- which DB2 refusing to rehydrate still leaks
	// through the timing.
	conditions := make([]string, 0, 4+len(request.Filters))
	arguments := make([]any, 0, 4+len(request.Filters))
	add := func(column, value string) {
		arguments = append(arguments, value)
		conditions = append(conditions, fmt.Sprintf("%s = $%d", column, len(arguments)+1))
	}
	if request.Workspace != "" {
		add("workspace", request.Workspace)
	}
	if request.Project != "" {
		add("project", request.Project)
	}
	if request.RecordType != "" {
		add("record_type", request.RecordType)
	}
	for _, filter := range request.Filters {
		column, allowed := labelColumn[filter.Key]
		if !allowed {
			return db3.SearchReply{}, fmt.Errorf(
				"postgres: no column for filter %q; refusing rather than ignoring it", filter.Key)
		}
		add(column, filter.Value)
	}

	where := ""
	if len(conditions) > 0 {
		where = " WHERE " + strings.Join(conditions, " AND ")
	}
	// $1 is the query vector and the last argument is the limit, so the filter
	// placeholders start at $2 -- which is what the offset in add() accounts for.
	query := fmt.Sprintf(
		"SELECT point_id, 1.0 - (%s <=> $1::vector) AS score FROM %s%s"+
			" ORDER BY %s <=> $1::vector LIMIT $%d",
		relation.vector, relation.table, where, relation.vector, len(arguments)+2)

	arguments = append([]any{vectorLiteral(request.Vector)}, arguments...)
	arguments = append(arguments, int64(request.TopK))

	rows, err := pool.Query(ctx, query, arguments...)
	if err != nil {
		return db3.SearchReply{}, fmt.Errorf("postgres: vector search: %w", err)
	}
	defer rows.Close()

	reply := db3.SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
	}
	for rows.Next() {
		var candidate db3.Candidate
		if err := rows.Scan(&candidate.PointID, &candidate.Score); err != nil {
			return db3.SearchReply{}, fmt.Errorf("postgres: reading a candidate: %w", err)
		}
		reply.Candidates = append(reply.Candidates, candidate)
	}
	if err := rows.Err(); err != nil {
		return db3.SearchReply{}, fmt.Errorf("postgres: vector search: %w", err)
	}
	return reply, nil
}

// vectorLiteral renders a vector the way pgvector parses it.
func vectorLiteral(vector []float32) string {
	var out strings.Builder
	out.WriteByte('[')
	for i, value := range vector {
		if i > 0 {
			out.WriteByte(',')
		}
		fmt.Fprintf(&out, "%g", value)
	}
	out.WriteByte(']')
	return out.String()
}
