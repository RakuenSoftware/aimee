package db2

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageProjectionEdges,
		db2contract.OperationProjectionEdges, projectionEdges)
	Register(db2contract.StageProjectionEdgesForGeneration,
		db2contract.OperationProjectionEdgesForGeneration, projectionEdgesForGeneration)
	Register(db2contract.StageCodeIndexOpRecord,
		db2contract.OperationCodeIndexOpRecord, codeIndexOpRecord)
	Register(db2contract.StageKBAuditAppend,
		db2contract.OperationKBAuditAppend, kbAuditAppend)
	Register(db2contract.StageBanditArmStatsRead,
		db2contract.OperationBanditArmStatsRead, banditArmStatsRead)
}

// The join to the generations table on state = 'visible' is what makes this the
// published graph rather than whatever a half-finished projection has written
// so far. A pending generation's edges are real rows; they are just not the
// answer to "what does this project look like".
const projectionEdgesQuery = `SELECT cpe.source, cpe.relation, cpe.target
 FROM code_projection_edges cpe
 JOIN code_projection_generations g ON g.id = cpe.generation_id
 JOIN projects p ON p.name = g.project
 WHERE cpe.project = $1 AND g.state = 'visible'
 AND p.lifecycle_state = 'current'
 ORDER BY cpe.source, cpe.target
 LIMIT $2`

// A total order over all three columns, which the C explains: if the LIMIT
// boundary cuts through edges sharing a source and target, an incomplete order
// makes the truncation depend on row ordering -- and the community partition
// derived from these edges would then depend on it too.
//
// The project read above orders by two columns only, so its truncation is not
// deterministic in the same way. That difference is the C's; it is not carried
// over here because widening the order changes which rows a truncated read
// returns, and that is a decision for whoever owns the projection.
const projectionEdgesForGenerationQuery = `SELECT source, relation, target
 FROM code_projection_edges
 WHERE generation_id = $1
 ORDER BY source, target, relation
 LIMIT $2`

// structuralWeight is how much a relation counts toward a node's structural
// importance.
//
// Defining something outweighs containing it, which outweighs merely calling
// it: a symbol's definition is where it lives, and a call is one of many. The
// scale is small and deliberate -- three, two, one -- because it is a tiebreak
// between edges rather than a measurement of anything.
//
// A relation nobody has weighted counts as one rather than zero. Unknown is not
// worthless, and a new relation type appearing should not make the edges
// carrying it vanish from a weighted walk.
func structuralWeight(relation string) uint32 {
	switch relation {
	case "defines":
		return 3
	case "contains", "exports", "routes", "depends_on":
		return 2
	default:
		return 1
	}
}

// readProjectionEdges runs one of the two edge reads.
func readProjectionEdges(ctx context.Context, store Store, query string,
	scope any, limit int,
) ([]db2contract.ProjectionEdgesRow, error) {
	rows, err := store.Query(ctx, query, scope, int64(limit))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	edges := make([]db2contract.ProjectionEdgesRow, 0, 16)
	for rows.Next() {
		var source, relation, target string
		if scanErr := rows.Scan(&source, &relation, &target); scanErr != nil {
			return nil, scanErr
		}
		edges = append(edges, db2contract.ProjectionEdgesRow{
			StructuralWeight:   structuralWeight(relation),
			ProjectionSource:   source,
			ProjectionRelation: relation,
			ProjectionTarget:   target,
		})
	}
	return edges, rows.Err()
}

// projectionEdges lists a project's published code graph.
func projectionEdges(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, limit, err := db2contract.DecodeProjectionEdgesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	edges, queryErr := readProjectionEdges(ctx, store, projectionEdgesQuery, project,
		pairLimit(limit, db2contract.ProjectionEdgesMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeProjectionEdgesReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// projectionEdgesForGeneration lists one generation's edges, published or not.
//
// Naming the generation is how a caller reads a projection that is not the
// visible one -- comparing a new generation against the current one, say, which
// is the whole point of building one before publishing it.
func projectionEdgesForGeneration(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	generation, limit, err :=
		db2contract.DecodeProjectionEdgesForGenerationRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	found, queryErr := readProjectionEdges(ctx, store,
		projectionEdgesForGenerationQuery, int64(generation),
		pairLimit(limit, db2contract.ProjectionEdgesForGenerationMaxRows))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	edges := make([]db2contract.ProjectionEdgesForGenerationRow, len(found))
	for index, edge := range found {
		edges[index] = db2contract.ProjectionEdgesForGenerationRow(edge)
	}
	reply, encodeErr :=
		db2contract.EncodeProjectionEdgesForGenerationReply(edges)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The same shape as the vector index record, for the same reasons: the attempt
// count grows on conflict so a retry policy can read how many times a point has
// been tried, and indexed_at is only set on success because a failed attempt
// indexed nothing.
const codeIndexOpRecordQuery = `INSERT INTO code_index_ops
 (point_id, project, node_key, file_path, status, attempts, last_error,
  indexed_at, updated_at)
 VALUES ($1, $2, $3, $4, $5, 1, $6,
  CASE WHEN $5 = 'ok' THEN pg_now_text() ELSE NULL END,
  pg_now_text())
 ON CONFLICT (point_id) DO UPDATE SET
  project    = excluded.project,
  node_key   = excluded.node_key,
  file_path  = excluded.file_path,
  status     = excluded.status,
  attempts   = code_index_ops.attempts + 1,
  last_error = excluded.last_error,
  indexed_at = excluded.indexed_at,
  updated_at = pg_now_text()`

// codeIndexOpRecord records the outcome of indexing one code node.
//
// Unlike the vector record, indexed_at is overwritten on conflict whether or
// not the new attempt succeeded -- so a point that indexed once and then failed
// loses the time it last worked. That is the C's behaviour and it is left
// alone, but it means indexed_at answers "when was this last attempted
// successfully" only for points that have never failed since.
func codeIndexOpRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pointID, project, nodeKey, filePath, indexOK, errorMessage, err :=
		db2contract.DecodeCodeIndexOpRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	status := "failed"
	if indexOK != 0 {
		status = "ok"
		errorMessage = ""
	}
	_, execErr := store.Exec(ctx, codeIndexOpRecordQuery,
		int64(pointID), project, nodeKey, filePath, status, errorMessage)
	return acknowledgement(execErr == nil,
		db2contract.EncodeCodeIndexOpRecordReply)
}

// The chain constants. The domain separates this hash from every other SHA-256
// in the tree, so a digest computed for something else can never be mistaken
// for a chain link, and the genesis value is what the first row links to.
const (
	auditWormDomain      = "aimee.audit.worm.v1"
	auditWormGenesisPrev = "0000000000000000000000000000000000000000000000000000000000000000"
)

const (
	// The tail of the chain, which decides both the next sequence number and
	// what the new row links back to.
	kbAuditTailQuery = `SELECT seq, row_hash FROM kb_audit_event
 ORDER BY seq DESC LIMIT 1`
	// key_id is written empty: this operation appends unsigned rows, and the
	// signing path fills that column elsewhere. It is part of the hashed
	// record, so writing anything else here would break verification.
	kbAuditInsertQuery = `INSERT INTO kb_audit_event
 (seq, ts, actor_role, actor_principal, action, subject, verdict, detail,
  key_id, prev_hash, row_hash)
 VALUES ($1, to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC',
   'YYYY-MM-DD"T"HH24:MI:SS"Z"'), $2, $3, $4, $5, $6, $7, '', $8, $9)`
)

// auditRowHash is the chain link over one record.
//
// Length-prefixed fields, in a fixed order, under a domain separator. The
// prefixes are what make the canonicalisation unambiguous: without them a
// record with an action of "ab" and a subject of "c" would hash the same as one
// with "a" and "bc", and an attacker who could choose two fields could move
// content between them without changing the digest.
func auditRowHash(seq int64, actorRole, actorPrincipal, action, subject,
	verdict, keyID, detail, prevHash string,
) string {
	var message strings.Builder
	message.WriteString(auditWormDomain)
	message.WriteByte('\n')
	message.WriteString(prevHash)
	message.WriteByte('\n')
	for _, field := range []string{
		strconv.FormatInt(seq, 10), actorRole, actorPrincipal, action,
		subject, verdict, keyID, detail,
	} {
		message.WriteString(strconv.Itoa(len(field)))
		message.WriteByte(':')
		message.WriteString(field)
	}
	digest := sha256.Sum256([]byte(message.String()))
	return hex.EncodeToString(digest[:])
}

// kbAuditAppend adds a row to the append-only audit chain.
//
// Read the tail, link to it, insert -- all in one transaction, as the C does.
// Two appenders racing still both read the same tail, and the sequence number
// is the table's primary key, so the loser fails rather than forking the chain.
// That is the property worth having: a refused append is visible, a forked
// chain is not.
//
// The database enforces the rest. Triggers on the table refuse every UPDATE,
// DELETE and TRUNCATE, so nothing that gets in can be quietly changed
// afterwards -- the hash chain proves the order, and the triggers protect the
// rows.
//
// The timestamp is written in the same UTC spelling the C's own formatter
// produces, and it is deliberately not part of the hashed record: the C hashes
// eight fields and the timestamp is not one of them.
func kbAuditAppend(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	actorRole, actorPrincipal, action, subject, verdict, detail, err :=
		db2contract.DecodeKBAuditAppendRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if action == "" {
		// The C refuses an empty action: an audit row that does not say what
		// happened is not evidence of anything.
		return acknowledgement(false, db2contract.EncodeKBAuditAppendReply)
	}

	txErr := store.InTx(ctx, func(tx Store) error {
		seq := int64(1)
		prev := auditWormGenesisPrev
		var tailSeq int64
		var tailHash string
		// An empty table is the genesis case rather than a failure, which is
		// why a missing tail is not an error here. A read that went wrong looks
		// the same and fails on the insert instead, where the sequence number
		// it would reuse is already taken.
		if scanErr := tx.QueryRow(ctx, kbAuditTailQuery).
			Scan(&tailSeq, &tailHash); scanErr == nil {
			seq, prev = tailSeq+1, tailHash
		}
		rowHash := auditRowHash(seq, actorRole, actorPrincipal, action, subject,
			verdict, "", detail, prev)
		_, execErr := tx.Exec(ctx, kbAuditInsertQuery, seq, actorRole,
			actorPrincipal, action, subject, verdict, detail, prev, rowHash)
		return execErr
	})
	return acknowledgement(txErr == nil, db2contract.EncodeKBAuditAppendReply)
}

const banditArmStatsReadQuery = `SELECT n_decisions, n_rewards, sum_reward,
 posterior_alpha, posterior_beta
 FROM bandit_arm_stats WHERE decision_point = $1 AND arm_id = $2`

// banditArmStatsRead answers what an arm has done so far.
//
// An arm nobody has pulled reads as the uninformative prior -- no decisions, no
// rewards, and Beta(1, 1) -- rather than as absent, which is what the reply can
// say: there is no found flag. The prior is the point. A sampler that draws
// from the posterior cannot draw from Beta(0, 0), which is not a distribution,
// so answering an untried arm with zeros would either crash the caller or make
// it silently skip the arm it has the most to learn from. Beta(1, 1) is uniform
// over [0, 1]: it says nothing about the arm, which is exactly what is known.
//
// The posteriors come back as stored rather than derived from the counts. They
// are the arm's parameters, which a sampler updates on its own schedule, and
// recomputing them here would answer a different question from the one the
// table holds.
func banditArmStatsRead(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionPoint, armID, err :=
		db2contract.DecodeBanditArmStatsReadRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var decisions, rewards int64
	var sumReward, alpha, beta float64
	if scanErr := store.QueryRow(ctx, banditArmStatsReadQuery,
		decisionPoint, armID).Scan(&decisions, &rewards, &sumReward,
		&alpha, &beta); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		decisions, rewards, sumReward = 0, 0, 0
		alpha, beta = 1, 1
	}
	if decisions < 0 {
		decisions = 0
	}
	if rewards < 0 {
		rewards = 0
	}
	reply, encodeErr := db2contract.EncodeBanditArmStatsReadReply(
		uint64(decisions), uint64(rewards), sumReward, alpha, beta)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
