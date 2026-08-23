package db2

import (
	"context"
	"strings"
	"unicode"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageAgentOutcomeRecord,
		db2contract.OperationAgentOutcomeRecord, agentOutcomeRecord)
	Register(db2contract.StageAntiPatternCheck,
		db2contract.OperationAntiPatternCheck, antiPatternCheck)
	Register(db2contract.StageArtifactListProposed,
		db2contract.OperationArtifactListProposed, artifactListProposed)
	Register(db2contract.StageArtifactWrite,
		db2contract.OperationArtifactWrite, artifactWrite)
	Register(db2contract.StageCuratorInvalidateDoc,
		db2contract.OperationCuratorInvalidateDoc, curatorInvalidateDoc)
}

const agentOutcomeRecordQuery = `INSERT INTO agent_outcomes
 (agent_name, role, outcome, reason, turns_used, tools_called, tokens_used,
  tool_error_pattern)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`

// agentOutcomeRecord records how an agent run ended and what it cost.
//
// Append-only: every run is a row, including the ones that ended the same way
// as the last. The table is read as a distribution -- how often this agent
// fails, how many turns it usually takes -- and deduplicating would make the
// common case invisible.
func agentOutcomeRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	name, role, outcome, reason, turns, tools, tokens, errorPattern, err :=
		db2contract.DecodeAgentOutcomeRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, agentOutcomeRecordQuery, name, role, outcome,
		reason, int64(turns), int64(tools), int64(tokens), errorPattern)
	return acknowledgement(execErr == nil,
		db2contract.EncodeAgentOutcomeRecordReply)
}

// Every anti-pattern, most confident first, matched in Go rather than in SQL.
//
// The matching is a word-bounded phrase comparison over normalised text, which
// is not something a LIKE can express: LIKE has no word boundaries, and adding
// them with a regular expression would mean escaping every pattern into one.
// The C reads the whole table and filters in C for the same reason.
const antiPatternCheckQuery = `SELECT id, pattern, description, source, source_ref,
 hit_count, confidence
 FROM anti_patterns ORDER BY confidence DESC`

// normalizeForMatch lowercases and collapses runs of whitespace to one space,
// trimming the ends.
//
// Both sides of the comparison go through it, which is what makes a pattern
// written with a newline in it match a command typed with a space.
func normalizeForMatch(text string) string {
	var out strings.Builder
	out.Grow(len(text))
	space := true
	for _, r := range text {
		if r == ' ' || r == '\t' || r == '\n' || r == '\r' {
			if !space {
				out.WriteByte(' ')
				space = true
			}
			continue
		}
		out.WriteRune(unicode.ToLower(r))
		space = false
	}
	return strings.TrimRight(out.String(), " ")
}

// isWordByte is what counts as inside a word for the boundary check.
func isWordByte(b byte) bool {
	return b == '_' || (b >= '0' && b <= '9') ||
		(b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
}

// phraseMatches answers whether target contains pattern as a word-bounded
// phrase. Both must already be normalised.
//
// The boundaries are what stop a pattern like "rm" matching "warm", and an
// empty pattern never matches -- a zero-length row would otherwise match
// everything, which is the failure mode of a table anyone can write to.
func phraseMatches(pattern, target string) bool {
	if pattern == "" || len(pattern) > len(target) {
		return false
	}
	for offset := 0; offset+len(pattern) <= len(target); offset++ {
		if target[offset:offset+len(pattern)] != pattern {
			continue
		}
		if offset > 0 && isWordByte(target[offset-1]) {
			continue
		}
		after := offset + len(pattern)
		if after < len(target) && isWordByte(target[after]) {
			continue
		}
		return true
	}
	return false
}

// antiPatternCheck answers which recorded anti-patterns a command trips.
//
// The file path and the command are joined into one target, so a pattern can
// name either or both -- "rm -rf" catches the command, and a pattern naming a
// path catches an edit to it.
func antiPatternCheck(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	filePath, command, err := db2contract.DecodeAntiPatternCheckRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	target := normalizeForMatch(filePath + " " + command)

	ceiling := db2contract.AntiPatternCheckMaxRows
	rows, queryErr := store.Query(ctx, antiPatternCheckQuery)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	tripped := make([]db2contract.AntiPatternCheckRow, 0, 8)
	for rows.Next() && len(tripped) < ceiling {
		var id, hits int64
		var pattern, description, source, sourceRef string
		var confidence float64
		if scanErr := rows.Scan(&id, &pattern, &description, &source,
			&sourceRef, &hits, &confidence); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		if !phraseMatches(normalizeForMatch(pattern), target) {
			continue
		}
		tripped = append(tripped, db2contract.AntiPatternCheckRow{
			AntiPatternID:      clampToU64(id),
			HitCount:           clampToU32(hits),
			Confidence:         confidence,
			Pattern:            pattern,
			PatternDescription: description,
			PatternSource:      source,
			SourceRef:          sourceRef,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeAntiPatternCheckReply(tripped)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The citation identifiers come back joined in one column rather than in a
// query per artifact. The C runs one statement for each row it returned, which
// is fifty statements for a full page; the aggregate is one.
//
// The ordering inside the aggregate is the C's, and it matters: the joined
// string is compared by callers, so an unordered aggregate would produce a
// different string for the same set of citations on different runs.
const artifactListProposedQuery = `SELECT a.id, a.kind, a.target_surface,
 a.confidence, a.created_at, a.payload,
 COALESCE((SELECT string_agg(c.source_id, ','
   ORDER BY c.source_kind, c.source_id, c.span_start, c.span_end)
   FROM artifact_citations c
   WHERE c.artifact_id = a.id AND c.source_id <> ''), '')
 FROM artifacts a
 WHERE a.state = 'proposed' AND ($1 = '' OR a.target_surface = $1)
 ORDER BY a.confidence DESC
 LIMIT $2`

// artifactListProposedDefaultLimit is what the C uses for a caller that asks
// for nothing.
const artifactListProposedDefaultLimit = 50

// artifactListProposed lists artifacts waiting for a decision.
//
// Most confident first, because a reviewer working down a list should meet the
// ones most likely to be right first -- the low-confidence tail is where
// reading stops being worth it.
//
// An empty surface means every surface, which is how a reviewer sees everything
// pending rather than having to name each one.
func artifactListProposed(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetSurface, listLimit, err :=
		db2contract.DecodeArtifactListProposedRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ArtifactListProposedMaxRows
	limit := artifactListProposedDefaultLimit
	if listLimit > 0 {
		limit = int(listLimit)
	}
	if limit > ceiling {
		limit = ceiling
	}

	rows, queryErr := store.Query(ctx, artifactListProposedQuery,
		targetSurface, int64(limit))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	proposed := make([]db2contract.ArtifactListProposedRow, 0, 16)
	for rows.Next() && len(proposed) < ceiling {
		var id, kind, surface, createdAt, payload, citations string
		var confidence float64
		if scanErr := rows.Scan(&id, &kind, &surface, &confidence, &createdAt,
			&payload, &citations); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		proposed = append(proposed, db2contract.ArtifactListProposedRow{
			ArtifactID:        id,
			ArtifactKind:      kind,
			ArtifactSurface:   surface,
			Confidence:        confidence,
			ArtifactCreatedAt: createdAt,
			PayloadJson:       payload,
			CitationIds:       citations,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeArtifactListProposedReply(proposed)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The identifier is the caller's here, unlike the artifact writes that mint
// one, so a caller that retries with the same identifier writes one artifact.
const artifactWriteQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 VALUES ($1, $2, $3, $4, $5, $6, $7, 1, '', '', '', '', pg_now_text(), $8::jsonb)
 ON CONFLICT (id) DO NOTHING`

// artifactWrite stores an artifact under an identifier the caller chose.
//
// The C emits MDL features after this write when the state is committed and the
// kind is "synthesis". That branch is not here: it is a second operation's
// worth of work triggered by a write, and the catalogue has no entry for it --
// so an artifact written through the module records itself and nothing more.
// Whoever owns the MDL pipeline has to trigger it explicitly at cutover.
func artifactWrite(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	artifactID, kind, state, scopeKind, scopeID, operatorID, confidence, payload, err :=
		db2contract.DecodeArtifactWriteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if state == "" {
		state = "proposed"
	}
	if scopeKind == "" {
		scopeKind = "user"
	}
	_, execErr := store.Exec(ctx, artifactWriteQuery, artifactID, kind, state,
		scopeKind, scopeID, operatorID, confidence, payload)
	return acknowledgement(execErr == nil, db2contract.EncodeArtifactWriteReply)
}

// One statement where the C runs a loop of loops.
//
// The C lists the file's chunk identifiers, then for each one drains a batch of
// citing artifacts, and for each of those writes a state change and an audit
// event -- so re-ingesting a file with forty chunks is at least eighty
// statements, and a failure part-way leaves some artifacts stale and others
// not.
//
// The chained CTEs do the same work in one round trip and one transaction:
// find the chunks, find the live artifacts citing any of them, stale those, and
// write an audit event for each. The count comes back from the same statement.
//
// The span predicate the C carries is not here, and that is not a loss: this
// caller always passes a whole-file edit, which makes the C's condition true
// for every citation regardless of span. Reproducing an always-true predicate
// would suggest it sometimes was not.
const curatorInvalidateDocQuery = `WITH chunks AS (
   SELECT d.id::text AS source_id
   FROM kb_documents d JOIN projects p ON p.name = d.project
   WHERE d.project = $1 AND d.file_path = $2
     AND p.lifecycle_state = 'current'
     AND d.generation = p.current_generation
), citing AS (
   SELECT DISTINCT a.id, a.state
   FROM artifacts a
   JOIN artifact_citations c ON c.artifact_id = a.id
   WHERE c.source_kind = 'kb_document'
     AND c.source_id IN (SELECT source_id FROM chunks)
     AND a.state IN ('proposed', 'committed')
), staled AS (
   UPDATE artifacts SET state = 'stale'
   FROM citing WHERE artifacts.id = citing.id
   RETURNING artifacts.id, citing.state AS prior_state
), audited AS (
   INSERT INTO audit_events
   (id, source_artifact_id, target_surface, target_id, operator_id,
    scope_kind, scope_id, applied_at, applied_confidence, flagged_for_review,
    before_snapshot, after_snapshot)
   SELECT gen_random_uuid()::text, staled.id, '', '', 'kb.curator.invalidate',
     'user', '', pg_now_text(), 0.0, false,
     jsonb_build_object('state', staled.prior_state),
     jsonb_build_object('state', 'stale', 'reason', 'cited_source_changed',
       'source', 'kb_document:' || staled.id)
   FROM staled
   RETURNING 1
)
SELECT count(*) FROM staled`

// The invalidation event, written only when something was actually
// invalidated. A file re-ingested with nothing citing it is not news.
const curatorInvalidationRecordQuery = `INSERT INTO curator_invalidation_events
 (source_kind, source_id, artifacts_stale) VALUES ('kb_file', $1, $2)`

// curatorInvalidateDoc marks every curator artifact citing a file's chunks as
// stale, and answers how many that was.
//
// One transaction, so a file is either fully invalidated or not at all. The C's
// loop can fail part-way and leave a document half-invalidated -- some
// artifacts stale, others still claiming to describe content that has changed
// -- which is worse than not having run, because it looks like it did.
func curatorInvalidateDoc(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, filePath, err :=
		db2contract.DecodeCuratorInvalidateDocRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var invalidated int64
	txErr := store.InTx(ctx, func(tx Store) error {
		if scanErr := tx.QueryRow(ctx, curatorInvalidateDocQuery, project, filePath).
			Scan(&invalidated); scanErr != nil {
			return scanErr
		}
		if invalidated == 0 {
			return nil
		}
		_, execErr := tx.Exec(ctx, curatorInvalidationRecordQuery,
			filePath, invalidated)
		return execErr
	})
	if txErr != nil {
		invalidated = 0
	}
	reply, encodeErr := db2contract.EncodeCuratorInvalidateDocReply(
		clampToU32(invalidated))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
