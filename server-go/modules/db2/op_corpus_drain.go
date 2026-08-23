package db2

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageCorpusPipelineDrain,
		db2contract.OperationCorpusPipelineDrain, corpusPipelineDrain)
}

// corpusDrainDefaultLimit is the C's cap when the caller names none. It is a
// backstop rather than a batch size: a drain that has taken ten thousand steps
// is looping over something.
const corpusDrainDefaultLimit = 10000

// The next document to move, locked.
//
// FOR UPDATE SKIP LOCKED is new. The C takes no lock at all, so two drains --
// and this is a pipeline built to be drained by whatever is running -- both
// read the same job, both run the same stage handler, and both advance it: the
// document skips a stage and the work is done twice.
//
// Ordered by document rather than by age, which is the C's order and worth
// keeping: it makes a drain walk one document to completion before starting
// the next, so a half-processed corpus is a prefix rather than a scatter.
const corpusClaimJobQuery = `SELECT doc_id, stage FROM corpus_processing_jobs
 WHERE stage_status IN ('pending','running') AND stage <> 'complete'
 ORDER BY doc_id ASC LIMIT 1
 FOR UPDATE SKIP LOCKED`

// The event log: what moved, from where to where, and what happened.
const corpusStageEventQuery = `INSERT INTO corpus_stage_events
 (doc_id, from_stage, to_stage, outcome, detail, worker, created_at)
 VALUES ($1, $2, $3, $4, $5, 'local', pg_now_text())`

const corpusAdvanceJobQuery = `UPDATE corpus_processing_jobs
 SET stage = $1, stage_status = $2, attempts = 0, last_error = '',
     updated_at = pg_now_text()
 WHERE doc_id = $3`

const corpusFailJobQuery = `UPDATE corpus_processing_jobs
 SET stage_status = 'failed', attempts = attempts + 1, last_error = $1,
     updated_at = pg_now_text()
 WHERE doc_id = $2`

// errCorpusDrainIdle says there was nothing left to move.
var errCorpusDrainIdle = errors.New("db2: no corpus job to drain")

// corpusPipelineDrain walks documents through the stages it can run locally and
// answers what the pipeline holds afterwards.
//
// The reply's processed and skipped counts are this drain's own, not the
// corpus's cumulative ones, which is the C's distinction and the useful one: a
// fresh drain of an already-skipped corpus would otherwise look busy.
func corpusPipelineDrain(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	drainLimit, err := db2contract.DecodeCorpusPipelineDrainRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	steps := int(drainLimit)
	if steps <= 0 {
		steps = corpusDrainDefaultLimit
	}
	processed, skipped := 0, 0
	for processed < steps {
		wasSkipped, stepErr := corpusDrainStep(ctx, store)
		if errors.Is(stepErr, errCorpusDrainIdle) {
			break
		}
		if stepErr != nil {
			// The job has been marked failed and the corpus_failed count in
			// the reply says so. The C returns a bare failure here and throws
			// away everything it had already processed -- so a drain of forty
			// documents that met one bad handler reported nothing at all, and
			// the caller could not tell that from a drain that did nothing.
			break
		}
		processed++
		if wasSkipped {
			skipped++
		}
	}
	return corpusDrainReply(ctx, store, processed, skipped)
}

// corpusDrainStep moves one document one stage, inside one transaction.
//
// The transaction is not the C's. Its handlers write directly -- the section
// rebuild deletes a document's sections before writing the new ones -- so a
// handler that failed part way left the document worse than it found it, and
// the job row still said pending.
func corpusDrainStep(ctx context.Context, store Store) (bool, error) {
	skipped := false
	err := store.InTx(ctx, func(tx Store) error {
		var docID int64
		var stage string
		switch scanErr := tx.QueryRow(ctx, corpusClaimJobQuery).Scan(&docID,
			&stage); {
		case scanErr == pgx.ErrNoRows:
			return errCorpusDrainIdle
		case scanErr != nil:
			return scanErr
		}
		nextStage, hasNext := corpusNextStage(stage)
		if !hasNext {
			// A stage with no successor is not an error and not progress. The
			// C stops the whole drain here; so does this.
			return errCorpusDrainIdle
		}
		detail, wasSkipped, handlerErr := runCorpusStage(ctx, tx, docID,
			nextStage)
		if handlerErr != nil {
			return handlerErr
		}
		skipped = wasSkipped
		outcome := "advanced"
		if wasSkipped {
			outcome = "skipped"
		}
		if _, execErr := tx.Exec(ctx, corpusStageEventQuery, docID, stage,
			nextStage, outcome, detail); execErr != nil {
			return execErr
		}
		status := "pending"
		if nextStage == "complete" {
			status = "complete"
		}
		_, execErr := tx.Exec(ctx, corpusAdvanceJobQuery, nextStage, status,
			docID)
		return execErr
	})
	if err != nil && !errors.Is(err, errCorpusDrainIdle) {
		markFailedCorpusJob(ctx, store, err)
	}
	return skipped, err
}

// corpusFailure carries the document a handler failed on, so the failure can be
// recorded after its transaction has been rolled back.
type corpusFailure struct {
	DocID  int64
	Detail string
}

func (failure *corpusFailure) Error() string {
	return fmt.Sprintf("db2: corpus stage failed for doc %d: %s", failure.DocID,
		failure.Detail)
}

// markFailedCorpusJob records the failure outside the transaction that failed.
//
// It has to be outside: the transaction that met the error is rolled back, and
// a failure written inside it would roll back with everything else -- which is
// how the C manages to leave a job pending after a handler that could never
// succeed, so the next drain picks it up and fails again.
func markFailedCorpusJob(ctx context.Context, store Store, cause error) {
	var failure *corpusFailure
	if !errors.As(cause, &failure) {
		return
	}
	detail := failure.Detail
	if detail == "" {
		detail = "stage handler failed"
	}
	_, _ = store.Exec(ctx, corpusFailJobQuery, detail, failure.DocID)
}

// runCorpusStage runs the handler for one stage, and says whether it did
// anything.
//
// A stage with no handler is skipped rather than failed. The pipeline is a
// ledger of where a document got to, and a stage this process cannot run is not
// a stage nothing can.
func runCorpusStage(ctx context.Context, tx Store, docID int64,
	stage string) (string, bool, error) {
	var detail string
	var err error
	switch stage {
	case "restore":
		// Reachable only through the restoration path, which parks a job here
		// for something else to pick up.
		return "restoration queued", true, nil
	case "classified":
		detail, err = classifyCorpusDoc(ctx, tx, docID, "pipeline")
	case "sectioned":
		detail, err = rebuildCorpusSections(ctx, tx, docID)
	case "references_extracted":
		detail, err = extractCorpusReferences(ctx, tx, docID)
	case "terms_normalized":
		detail, err = normalizeCorpusTerms(ctx, tx, docID)
	case "gaps_detected":
		detail, err = detectCorpusGaps(ctx, tx, docID)
	default:
		return "no local handler", true, nil
	}
	if err != nil {
		return "", false, &corpusFailure{DocID: docID, Detail: err.Error()}
	}
	return detail, false, nil
}

// corpusDrainReply reads the pipeline's counts and answers with this drain's
// own processed and skipped figures written over them.
func corpusDrainReply(ctx context.Context, store Store, processed,
	skipped int) ([]byte, bus.ModuleStatus) {
	var total, pending, running, failed, complete, cumulativeSkipped int64
	if err := store.QueryRow(ctx, corpusPipelineStatusQuery).Scan(&total,
		&pending, &running, &failed, &complete,
		&cumulativeSkipped); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	// The first field is not a count. The envelope bounds it to zero or one,
	// and the adapter sets it when the drain ran at all -- so it says "this
	// reply is a drain's reply", and corpus_processed carries how much moved.
	reply, encodeErr := db2contract.EncodeCorpusPipelineDrainReply(
		1, uint32(total), uint32(pending), uint32(running),
		uint32(failed), uint32(complete), uint32(processed), uint32(skipped))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// corpusGapCap is the C's MAX_GAPS: the most gaps one pass will raise.
const corpusGapCap = 20

// Entities nothing has ever cited from a document section.
//
// An entity the corpus asserts and never sources is the definition of an
// undefined one, and it is worth a question rather than a correction: the
// citation may simply never have been recorded.
const corpusUndefinedEntitiesQuery = `SELECT e.id FROM artifacts e
 WHERE e.kind = 'entity' AND e.state = 'committed'
   AND NOT EXISTS (SELECT 1 FROM artifact_citations ac
     WHERE ac.artifact_id = e.id AND ac.source_kind = 'doc_section')
 ORDER BY e.id LIMIT $1`

// References this document makes that resolve to nothing.
const corpusDanglingReferencesQuery = `SELECT id, raw_target
 FROM document_references
 WHERE from_doc_id = $1 AND resolution IN ('unresolved','stale')
 ORDER BY id LIMIT $2`

// The gaps that are not already raised, answered for the whole batch rather
// than one existence check per subject.
const corpusKnownGapsQuery = `SELECT candidate.subject
 FROM unnest($1::text[]) AS candidate(subject)
 WHERE EXISTS (SELECT 1 FROM artifacts
   WHERE kind = 'gap' AND state <> 'retired'
     AND payload->>'subject' = candidate.subject
     AND payload->>'gap_kind' = $2)`

const corpusWriteGapsQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 SELECT gap.id, 'gap', 'proposed', 'global', 'global', 'corpus.gaps', 0.6, 1,
        '', '', '', '', pg_now_text(), gap.payload::jsonb
   FROM unnest($1::text[], $2::text[]) AS gap(id, payload)
 ON CONFLICT (id) DO NOTHING`

// The curiosity item a gap is promoted to, unless the subject already has one
// open.
//
// The item is what makes a gap actionable: the artifact records that something
// is missing, and this is the thing that will be worked on. A subject that
// already has an open item does not get a second one -- the same absence
// noticed twice is one question.
const corpusPromoteGapsQuery = `INSERT INTO curiosity_items
 (gap_type, target_entity, target_topic, evidence, importance, novelty, state,
  source_session, created_at, updated_at)
 SELECT $1, promotion.subject, promotion.subject, promotion.evidence, 0.5, 0.7,
        'open', 'corpus.gaps', pg_now_text(), pg_now_text()
   FROM unnest($2::text[], $3::text[]) AS promotion(subject, evidence)
  WHERE NOT EXISTS (SELECT 1 FROM curiosity_items existing
    WHERE existing.target_entity = promotion.subject
      AND existing.state NOT IN ('resolved','suppressed'))`

// corpusGapKinds maps a gap kind to the curiosity gap type it becomes.
//
// A dangling reference is weak coverage rather than a missing fact: the corpus
// points at something it does not hold, which is a hole in what it covers, not
// a claim it cannot support.
var corpusGapKinds = map[string]string{
	"undefined_entity":   "missing_fact",
	"dangling_reference": "weak_coverage",
}

// detectCorpusGaps raises what the corpus is missing: entities it asserts and
// never sources, and references it makes and cannot resolve.
func detectCorpusGaps(ctx context.Context, tx Store, docID int64) (string, error) {
	raised := 0
	entities, err := readCorpusGapSubjects(ctx, tx,
		corpusUndefinedEntitiesQuery, corpusGapCap)
	if err != nil {
		return "", err
	}
	written, writeErr := writeCorpusGaps(ctx, tx, "undefined_entity", entities)
	if writeErr != nil {
		return "", writeErr
	}
	raised += written

	dangling, danglingErr := readCorpusDanglingGaps(ctx, tx, docID)
	if danglingErr != nil {
		return "", danglingErr
	}
	written, writeErr = writeCorpusGaps(ctx, tx, "dangling_reference", dangling)
	if writeErr != nil {
		return "", writeErr
	}
	raised += written
	return fmt.Sprintf("gaps=%d", raised), nil
}

// corpusGap is one absence: what is missing, and what says so.
type corpusGap struct {
	Subject  string
	Evidence string
}

func readCorpusGapSubjects(ctx context.Context, tx Store, query string,
	limit int) ([]corpusGap, error) {
	rows, err := tx.Query(ctx, query, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	gaps := []corpusGap{}
	for rows.Next() {
		var subject string
		if scanErr := rows.Scan(&subject); scanErr != nil {
			return nil, scanErr
		}
		if subject == "" {
			continue
		}
		// The entity is its own evidence: the artifact that asserts it is
		// what a reader has to look at to see the gap.
		gaps = append(gaps, corpusGap{Subject: subject, Evidence: subject})
	}
	return gaps, rows.Err()
}

func readCorpusDanglingGaps(ctx context.Context, tx Store, docID int64) (
	[]corpusGap, error,
) {
	rows, err := tx.Query(ctx, corpusDanglingReferencesQuery, docID,
		corpusGapCap)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	gaps := []corpusGap{}
	for rows.Next() {
		var referenceID int64
		var rawTarget string
		if scanErr := rows.Scan(&referenceID, &rawTarget); scanErr != nil {
			return nil, scanErr
		}
		if rawTarget == "" {
			continue
		}
		gaps = append(gaps, corpusGap{
			Subject:  rawTarget,
			Evidence: fmt.Sprintf("document_reference:%d", referenceID),
		})
	}
	return gaps, rows.Err()
}

// writeCorpusGaps records the gaps nobody has raised yet and promotes them.
func writeCorpusGaps(ctx context.Context, tx Store, gapKind string,
	gaps []corpusGap) (int, error) {
	if len(gaps) == 0 {
		return 0, nil
	}
	subjects := make([]string, len(gaps))
	for index, gap := range gaps {
		subjects[index] = gap.Subject
	}
	known, knownErr := readKnownCorpusGaps(ctx, tx, gapKind, subjects)
	if knownErr != nil {
		return 0, knownErr
	}
	identifiers := []string{}
	payloads := []string{}
	freshSubjects := []string{}
	evidence := []string{}
	for _, gap := range gaps {
		if known[gap.Subject] {
			continue
		}
		artifactID, idErr := newArtifactID()
		if idErr != nil {
			return 0, idErr
		}
		payload, payloadErr := json.Marshal(map[string]any{
			"subject":       gap.Subject,
			"gap_kind":      gapKind,
			"evidence_refs": []string{gap.Evidence},
		})
		if payloadErr != nil {
			return 0, payloadErr
		}
		identifiers = append(identifiers, artifactID)
		payloads = append(payloads, string(payload))
		freshSubjects = append(freshSubjects, gap.Subject)
		evidence = append(evidence, gap.Evidence)
	}
	if len(identifiers) == 0 {
		return 0, nil
	}
	if _, execErr := tx.Exec(ctx, corpusWriteGapsQuery, identifiers,
		payloads); execErr != nil {
		return 0, execErr
	}
	if _, execErr := tx.Exec(ctx, corpusPromoteGapsQuery,
		corpusGapKinds[gapKind], freshSubjects, evidence); execErr != nil {
		return 0, execErr
	}
	return len(identifiers), nil
}

func readKnownCorpusGaps(ctx context.Context, tx Store, gapKind string,
	subjects []string) (map[string]bool, error) {
	rows, err := tx.Query(ctx, corpusKnownGapsQuery, subjects, gapKind)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	known := map[string]bool{}
	for rows.Next() {
		var subject string
		if scanErr := rows.Scan(&subject); scanErr != nil {
			return nil, scanErr
		}
		known[subject] = true
	}
	return known, rows.Err()
}
