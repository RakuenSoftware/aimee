package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageGetContent,
		db2contract.OperationGetContent, getContent)
	Register(db2contract.StageGetSourceSession,
		db2contract.OperationGetSourceSession, getSourceSession)
	Register(db2contract.StagePickFirstTemporalRef,
		db2contract.OperationPickFirstTemporalRef, pickFirstTemporalRef)
	Register(db2contract.StageCountAndMaxUpdated,
		db2contract.OperationCountAndMaxUpdated, countAndMaxUpdated)
	Register(db2contract.StageStatsCounts,
		db2contract.OperationStatsCounts, statsCounts)
}

const (
	getContentQuery = `SELECT content FROM memories WHERE id = $1`

	getSourceSessionQuery = `SELECT source_session FROM memories WHERE id = $1`

	// The granularity order is the C's and it is a preference, not an
	// alphabet: a phrase a person actually wrote ("last Tuesday") beats a date
	// the extractor derived, and a year is the weakest thing worth keeping.
	// Weight breaks ties within a granularity and the identifier breaks those,
	// so the same memory answers the same reference every time.
	pickFirstTemporalRefQuery = `SELECT ref_key FROM memory_temporal_refs
 WHERE memory_id = $1
 ORDER BY CASE granularity
            WHEN 'date_phrase' THEN 0
            WHEN 'absolute_day' THEN 1
            WHEN 'month' THEN 2
            WHEN 'weekday' THEN 3
            WHEN 'year' THEN 4
            ELSE 5 END,
          weight DESC, id ASC
 LIMIT 1`

	// MAX over an empty table is NULL, which is not an error and not a stamp.
	// COALESCE makes it the empty string the reply carries for "no memories".
	countAndMaxUpdatedQuery = `SELECT COUNT(*), COALESCE(MAX(updated_at), '')
 FROM memories`

	// One statement for what the C reads in three, and the counts are pivoted
	// here rather than in Go: the C walks a GROUP BY and assigns each row into
	// a fixed slot by name, which is a pivot written as a loop.
	statsCountsQuery = `SELECT
 COUNT(*) FILTER (WHERE tier = 'L0'), COUNT(*) FILTER (WHERE tier = 'L1'),
 COUNT(*) FILTER (WHERE tier = 'L2'), COUNT(*) FILTER (WHERE tier = 'L3'),
 COUNT(*) FILTER (WHERE tier = 'L4'), COUNT(*) FILTER (WHERE tier = 'L5'),
 COUNT(*) FILTER (WHERE kind = 'fact'),
 COUNT(*) FILTER (WHERE kind = 'preference'),
 COUNT(*) FILTER (WHERE kind = 'decision'),
 COUNT(*) FILTER (WHERE kind = 'episode'),
 COUNT(*) FILTER (WHERE kind = 'task'),
 COUNT(*) FILTER (WHERE kind = 'scratch'),
 COUNT(*) FILTER (WHERE kind = 'procedure'),
 COUNT(*) FILTER (WHERE kind = 'policy'),
 COUNT(*) FILTER (WHERE kind = 'workflow'),
 COUNT(*) FILTER (WHERE kind = 'opinion'),
 COUNT(*),
 (SELECT COUNT(*) FROM memory_conflicts WHERE resolved = 0)
 FROM memories`
)

// getContent answers a memory's text, or says it is not there.
func getContent(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeGetContentRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return textReply(ctx, store, getContentQuery,
		db2contract.EncodeGetContentReply, int64(memoryID))
}

func getSourceSession(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodeGetSourceSessionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return textReply(ctx, store, getSourceSessionQuery,
		db2contract.EncodeGetSourceSessionReply, int64(memoryID))
}

// pickFirstTemporalRef answers the strongest time reference a memory carries.
func pickFirstTemporalRef(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, err := db2contract.DecodePickFirstTemporalRefRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return textReply(ctx, store, pickFirstTemporalRefQuery,
		db2contract.EncodePickFirstTemporalRefReply, int64(memoryID))
}

// countAndMaxUpdated answers how many memories there are and when the newest
// was written.
//
// A failure is invalid_state rather than not_found, which is the C's
// distinction and the right one: the aggregate always yields a row when it
// runs, so no row means it did not run.
func countAndMaxUpdated(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeCountAndMaxUpdatedRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var count int64
	var stamp *string
	if err := store.QueryRow(ctx, countAndMaxUpdatedQuery).
		Scan(&count, &stamp); err != nil {
		reply, encodeErr := db2contract.EncodeCountAndMaxUpdatedReply(
			db2contract.ResultInvalidState, 0, "")
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	reply, encodeErr := db2contract.EncodeCountAndMaxUpdatedReply(
		db2contract.ResultOK, uint32(max(count, 0)), text(stamp))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// statsCounts answers the corpus by tier, by kind, in total, and how much of it
// is in unresolved conflict.
//
// The slots are positional and their order is the contract's, so a tier or kind
// the schema grows later lands nowhere until the contract makes room -- which
// is why the counts are named in the statement rather than pivoted from
// whatever names the table happens to hold.
func statsCounts(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeStatsCountsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	counts := make([]int64, db2contract.StatsCountsTiers+
		db2contract.StatsCountsKinds+2)
	targets := make([]any, len(counts))
	for index := range counts {
		targets[index] = &counts[index]
	}
	if err := store.QueryRow(ctx, statsCountsQuery).Scan(targets...); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	var stats db2contract.MemoryStats
	for index := range stats.TierCounts {
		stats.TierCounts[index] = uint32(max(counts[index], 0))
	}
	for index := range stats.KindCounts {
		stats.KindCounts[index] = uint32(
			max(counts[db2contract.StatsCountsTiers+index], 0))
	}
	stats.Total = uint32(max(counts[len(counts)-2], 0))
	stats.Conflicts = uint32(max(counts[len(counts)-1], 0))
	reply, encodeErr := db2contract.EncodeStatsCountsReply(stats)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// textReply answers a reply of a result domain and one string.
//
// No row is not_found rather than an error: the caller asked about an
// identifier and the honest answer is that nothing is under it.
func textReply(ctx context.Context, store Store, query string,
	encode func(uint32, string) ([]byte, error), args ...any) (
	[]byte, bus.ModuleStatus,
) {
	// A pointer target, because every column these read is nullable: a memory
	// with no source session holds NULL there, and scanning that into a string
	// fails the read rather than answering the empty string it means.
	var value *string
	switch err := store.QueryRow(ctx, query, args...).Scan(&value); {
	case errors.Is(err, pgx.ErrNoRows):
		reply, encodeErr := encode(db2contract.ResultNotFound, "")
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	case err != nil:
		return nil, bus.ModuleStatusInternal
	}
	if text(value) == "" {
		// The contract makes an OK reply with an empty string unrepresentable:
		// a session id, a content body and a reference key all declare a
		// minimum length of one. An empty column therefore has to answer
		// not_found, which is the same thing the field means -- there is
		// nothing here -- rather than an encode failure the caller reads as an
		// internal fault.
		reply, encodeErr := encode(db2contract.ResultNotFound, "")
		if encodeErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}
	reply, encodeErr := encode(db2contract.ResultOK, text(value))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
