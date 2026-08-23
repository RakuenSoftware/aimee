package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageL2CrossKeyPairs,
		db2contract.OperationL2CrossKeyPairs, l2CrossKeyPairs)
	Register(db2contract.StageL2FactDecisionPairs,
		db2contract.OperationL2FactDecisionPairs, l2FactDecisionPairs)
	Register(db2contract.StageMemorySummariseClusters,
		db2contract.OperationMemorySummariseClusters, memorySummariseClusters)
	Register(db2contract.StageMemoryPriorInSession,
		db2contract.OperationMemoryPriorInSession, memoryPriorInSession)
}

// A self-join looking for two memories under different keys that talk about
// each other, which is how a contradiction sweep finds candidates worth
// comparing.
//
// a.id < b.id rather than a.id != b.id: the pairing is symmetric, so without
// the ordering every pair comes back twice with the sides swapped.
//
// The prefix each side is matched against is the key up to its first
// underscore, and the concatenated underscore is what makes a key with none
// still yield a prefix rather than an empty string -- STRPOS would answer zero
// and SUBSTR would take nothing.
//
// The asymmetry in the LOWER calls is the C's: the haystack is lowered and the
// needle is not, so a key with a capital in it matches nothing here. Keys are
// written lowercase by every writer in the tree, which is why it has never
// shown; it is preserved rather than quietly fixed because the fix would widen
// what a contradiction sweep pairs up.
const l2CrossKeyPairsQuery = `SELECT a.id, b.id, a.content, b.content
 FROM memories a, memories b
 WHERE a.tier = 'L2' AND b.tier = 'L2'
 AND a.id < b.id
 AND a.key != b.key
 AND a.confidence > 0.5 AND b.confidence > 0.5
 AND (
   LOWER(a.key || ' ' || a.content) LIKE
     '%' || SUBSTR(b.key, 1, STRPOS(b.key || '_', '_') - 1) || '%'
   OR LOWER(b.key || ' ' || b.content) LIKE
     '%' || SUBSTR(a.key, 1, STRPOS(a.key || '_', '_') - 1) || '%'
 )
 LIMIT $1`

// The other half of the same sweep: a fact and a decision that mention each
// other by key, where one may have been superseded by the other.
//
// f.id != d.id rather than f.id < d.id, because the two sides are not
// interchangeable here -- a fact paired with a decision is a different finding
// from a decision paired with a fact, and the tier ranges differ too.
const l2FactDecisionPairsQuery = `SELECT f.id, d.id, f.content, d.content
 FROM memories f, memories d
 WHERE f.kind = 'fact' AND d.kind = 'decision'
 AND f.tier IN ('L1', 'L2') AND d.tier IN ('L2', 'L3')
 AND f.id != d.id
 AND f.confidence > 0.5 AND d.confidence > 0.5
 AND (LOWER(f.content) LIKE '%' || LOWER(d.key) || '%'
      OR LOWER(d.content) LIKE '%' || LOWER(f.key) || '%')
 LIMIT $1`

// readMemoryPairs runs one of the two pair statements.
//
// Both LIKE patterns are built from a key rather than from a parameter, so a
// key containing a percent or an underscore is a wildcard rather than a
// literal. That is the C's behaviour and it widens what the sweep pairs up
// rather than breaking it: the sweep proposes candidates, and something else
// decides whether they actually contradict.
func readMemoryPairs(ctx context.Context, store Store, query string, limit int) (
	[]db2contract.L2CrossKeyPairsRow, error,
) {
	rows, err := store.Query(ctx, query, int64(limit))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	pairs := make([]db2contract.L2CrossKeyPairsRow, 0, 16)
	for rows.Next() {
		var idA, idB int64
		var contentA, contentB string
		if scanErr := rows.Scan(&idA, &idB, &contentA, &contentB); scanErr != nil {
			return nil, scanErr
		}
		pairs = append(pairs, db2contract.L2CrossKeyPairsRow{
			MemoryIDA: uint64(idA),
			MemoryIDB: uint64(idB),
			ContentA:  contentA,
			ContentB:  contentB,
		})
	}
	return pairs, rows.Err()
}

// pairLimit clamps the caller's request to what the reply can carry.
//
// Zero asks for the ceiling rather than for nothing, which is what the C's
// prior-in-session read does with a limit of zero or less. The two pair sweeps
// never exercise either branch: their envelope bounds max_pairs from one to the
// reply's own row ceiling, so the clamp is already applied before the request
// decodes. It is shared anyway rather than written once, because the bound
// belonging to the envelope is a fact about today's schema.
func pairLimit(requested uint32, ceiling int) int {
	if requested == 0 || int(requested) > ceiling {
		return ceiling
	}
	return int(requested)
}

// l2CrossKeyPairs proposes pairs of long-term memories that may contradict.
func l2CrossKeyPairs(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	maxPairs, err := db2contract.DecodeL2CrossKeyPairsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	pairs, queryErr := readMemoryPairs(ctx, store, l2CrossKeyPairsQuery,
		pairLimit(maxPairs, db2contract.L2CrossKeyPairsMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeL2CrossKeyPairsReply(pairs)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// l2FactDecisionPairs proposes facts and decisions that may contradict.
func l2FactDecisionPairs(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	maxPairs, err := db2contract.DecodeL2FactDecisionPairsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	found, queryErr := readMemoryPairs(ctx, store, l2FactDecisionPairsQuery,
		pairLimit(maxPairs, db2contract.L2FactDecisionPairsMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	pairs := make([]db2contract.L2FactDecisionPairsRow, len(found))
	for index, pair := range found {
		pairs[index] = db2contract.L2FactDecisionPairsRow(pair)
	}
	reply, encodeErr := db2contract.EncodeL2FactDecisionPairsReply(pairs)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// merged_into = 0 is the whole reason this can be run repeatedly: a memory
// already folded into another is not a candidate for folding again, and without
// that clause every pass would re-propose the same clusters.
//
// source_session IS NOT NULL where the C skips a NULL session after reading it.
// Same rows, but as a predicate it also keeps the NULL group from occupying a
// row of the limit -- and a memory with no session is not a session's cluster.
//
// COUNT(*) is repeated in the HAVING rather than named by its alias, which the
// C carries a note about: PostgreSQL evaluates HAVING before the select list.
const memorySummariseClustersQuery = `SELECT source_session, COUNT(*) AS cluster_count,
 AVG(confidence) AS average_confidence
 FROM memories
 WHERE tier = 'L1' AND confidence <= $1 AND kind = 'fact' AND merged_into = 0
 AND source_session IS NOT NULL
 GROUP BY source_session
 HAVING COUNT(*) >= $2
 LIMIT $3`

// memorySummariseClusters finds sessions with enough low-confidence facts to be
// worth consolidating into one.
//
// The confidence ceiling comes from the caller, and it decides how much of a
// session gets folded: a high one folds nearly everything the session produced.
// Nothing here refuses that.
func memorySummariseClusters(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	maxConfidence, minCount, err :=
		db2contract.DecodeMemorySummariseClustersRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemorySummariseClustersMaxRows
	rows, queryErr := store.Query(ctx, memorySummariseClustersQuery,
		maxConfidence, int64(minCount), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	clusters := make([]db2contract.MemorySummariseClustersRow, 0, 16)
	for rows.Next() {
		var session string
		var count int64
		var average float64
		if scanErr := rows.Scan(&session, &count, &average); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		clusters = append(clusters, db2contract.MemorySummariseClustersRow{
			SessionID:         session,
			ClusterCount:      uint32(count),
			AverageConfidence: average,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemorySummariseClustersReply(clusters)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Descending, so the most recent prior memory leads. Identifiers are handed out
// in order, so id < the one asked about is "before" without needing a
// timestamp -- and it stays true for two memories written in the same second,
// which a timestamp comparison would not.
const memoryPriorInSessionQuery = `SELECT content, key FROM memories
 WHERE source_session = $1 AND id < $2
 ORDER BY id DESC LIMIT $3`

// memoryPriorInSession lists what a session had already recorded before a given
// memory.
//
// The context a memory was written against, which is what makes it possible to
// ask whether it repeats or contradicts something the same session already
// said.
func memoryPriorInSession(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	sessionID, beforeID, rowLimit, err :=
		db2contract.DecodeMemoryPriorInSessionRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryPriorInSessionMaxRows
	rows, queryErr := store.Query(ctx, memoryPriorInSessionQuery,
		sessionID, int64(beforeID), int64(pairLimit(rowLimit, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	prior := make([]db2contract.MemoryPriorInSessionRow, 0, 16)
	for rows.Next() {
		var content, key string
		if scanErr := rows.Scan(&content, &key); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		prior = append(prior, db2contract.MemoryPriorInSessionRow{
			MemoryContent: content,
			MemoryKey:     key,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryPriorInSessionReply(prior)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
