package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageArtifactWriteEvidence,
		db2contract.OperationArtifactWriteEvidence, artifactWriteEvidence)
	Register(db2contract.StageBanditDecisionInsert,
		db2contract.OperationBanditDecisionInsert, banditDecisionInsert)
	Register(db2contract.StageCalibrationProfileWrite,
		db2contract.OperationCalibrationProfileWrite, calibrationProfileWrite)
	Register(db2contract.StageCalibrationConformalWindow,
		db2contract.OperationCalibrationConformalWindow, calibrationConformalWindow)
	Register(db2contract.StageFeatureRowUpsert,
		db2contract.OperationFeatureRowUpsert, featureRowUpsert)
	Register(db2contract.StageLearningProposalInsert,
		db2contract.OperationLearningProposalInsert, learningProposalInsert)
}

// Idempotent capture: the same evidence, meaning the same kind and the same
// content hash, collapses onto the row that is already there. Re-uploading a
// session or a feedback batch is the normal case, and duplicating it would
// weight the same observation twice everywhere evidence is counted.
//
// The hash lives in source_bundle_hash, which is what the C uses and what the
// existing rows carry.
const (
	artifactEvidenceExistingQuery = `SELECT id FROM artifacts
 WHERE kind = $1 AND source_bundle_hash = $2 LIMIT 1`
	artifactEvidenceInsertQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 VALUES ($1, $2, 'proposed', $3, $4, $5, 1.0, 1, $6, '', '', '',
  pg_now_text(), $7::jsonb)
 ON CONFLICT (id) DO NOTHING`
)

// artifactWriteEvidence records a batch of evidence and answers its identifier.
//
// Evidence is proposed rather than committed: it is an observation, and what it
// is worth is decided by whatever reads it.
//
// An empty content hash skips the deduplication entirely, which is the C's
// behaviour and the right one -- an unhashed batch cannot be compared against
// anything, so treating every unhashed batch as the same one would collapse
// them all onto the first.
func artifactWriteEvidence(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	kind, scopeKind, scopeID, operatorID, contentHash, payload, err :=
		db2contract.DecodeArtifactWriteEvidenceRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if scopeKind == "" {
		scopeKind = "user"
	}

	if contentHash != "" {
		var existing string
		if scanErr := store.QueryRow(ctx, artifactEvidenceExistingQuery,
			kind, contentHash).Scan(&existing); scanErr == nil {
			return evidenceReply(existing)
		}
	}
	artifactID, idErr := newArtifactID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	if _, execErr := store.Exec(ctx, artifactEvidenceInsertQuery, artifactID,
		kind, scopeKind, scopeID, operatorID, contentHash, payload); execErr != nil {
		artifactID = ""
	}
	return evidenceReply(artifactID)
}

func evidenceReply(artifactID string) ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeArtifactWriteEvidenceReply(artifactID)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The decision identifier is the caller's, so a retry of the same decision
// records it once. ON CONFLICT DO NOTHING rather than an update: a decision is
// a thing that happened, and the second telling of it is not new information.
const banditDecisionInsertQuery = `INSERT INTO bandit_decisions
 (id, decision_point, arm_id, context_hash, propensity, decided_at, is_exploration)
 VALUES ($1, $2, $3, $4, $5, pg_now_text(), $6)
 ON CONFLICT (id) DO NOTHING`

// banditDecisionInsert records which arm was pulled and how likely that was.
//
// The propensity is what makes the log usable afterwards: an outcome from an
// arm chosen with probability a tenth counts differently from the same outcome
// on an arm chosen almost always, and without the probability at decision time
// there is no way to recover that.
//
// is_exploration is a boolean, and a boolean is what goes into it. The C binds a
// double there, which works only because libpq renders it as text and
// PostgreSQL accepts some spellings of a number as a boolean literal; pgx sends
// the parameter typed, so a float would be refused outright. It answers whether
// the arm was chosen to learn something rather than because it looked best -- a
// distinction the propensity alone does not carry.
func banditDecisionInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionID, decisionPoint, armID, contextHash, propensity, exploration, err :=
		db2contract.DecodeBanditDecisionInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	explorationValue := exploration != 0
	_, execErr := store.Exec(ctx, banditDecisionInsertQuery, decisionID,
		decisionPoint, armID, contextHash, propensity, explorationValue)
	return acknowledgement(execErr == nil,
		db2contract.EncodeBanditDecisionInsertReply)
}

// One insert where the C writes and then stamps.
//
// The C reaches the artifacts table through its generic writer, which has no
// parameter for target_surface, model_version or prompt_version, so it inserts
// and then updates the row it just wrote. Those three carry the surface, the
// artifact kind and the feature set version -- which is how a profile is found
// again -- so the window between the two writes is a window in which the
// profile exists and nothing can find it.
const calibrationProfileWriteQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, committed_at, payload)
 VALUES ($1, 'calibration_profile', 'committed', $2, $3, '', 1.0, 1, '',
  $4, $5, $6, pg_now_text(), pg_now_text(), $7::jsonb)
 ON CONFLICT (id) DO NOTHING`

// calibrationProfileWrite stores a calibration profile and answers its
// identifier.
//
// Three columns are carrying values they were not named for, and the C does the
// same: model_version holds the artifact kind being calibrated, prompt_version
// holds the feature set version, and target_surface holds the surface. The
// alternative was three more columns on a table many kinds share.
//
// An absent feature set version becomes "v1" rather than empty, which is the
// C's default and matters because the version is part of how a profile is
// matched: an empty one would never match a reader asking for a version.
func calibrationProfileWrite(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetSurface, artifactKind, scopeKind, scopeID, featureSetVersion, payload, err :=
		db2contract.DecodeCalibrationProfileWriteRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if scopeKind == "" {
		scopeKind = "global"
	}
	if featureSetVersion == "" {
		featureSetVersion = "v1"
	}
	artifactID, idErr := newArtifactID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	if _, execErr := store.Exec(ctx, calibrationProfileWriteQuery, artifactID,
		scopeKind, scopeID, artifactKind, featureSetVersion, targetSurface,
		payload); execErr != nil {
		artifactID = ""
	}
	reply, encodeErr :=
		db2contract.EncodeCalibrationProfileWriteReply(artifactID)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Only judged events, and only the two verdicts that say whether the confidence
// was warranted. An abstention is not evidence either way, and including it
// would drag the calibration toward the middle.
//
// The scope filter is a parameter pair rather than three statements: an empty
// scope kind admits every scope, and an empty scope id admits every identifier
// within the kind. The C builds three statement texts to say the same thing.
const calibrationConformalWindowQuery = `SELECT ae.applied_confidence, ae.verdict
 FROM audit_events ae
 JOIN artifacts a ON a.id = ae.source_artifact_id
 WHERE a.kind = $1
   AND ae.target_surface = $2
   AND ae.verdict IN ('accepted', 'rejected')
   AND ($3 = '' OR ae.scope_kind = $3)
   AND ($3 = '' OR $4 = '' OR ae.scope_id = $4)
 ORDER BY ae.applied_at DESC
 LIMIT $5`

// calibrationConformalWindow reads the recent judgements a surface's confidence
// can be calibrated against.
//
// Newest first, because calibration is about how well the current model is
// doing: a window that reached back to the oldest judgements would measure a
// model that has since been replaced.
func calibrationConformalWindow(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	targetSurface, artifactKind, scopeKind, scopeID, windowRows, err :=
		db2contract.DecodeCalibrationConformalWindowRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.CalibrationConformalWindowMaxRows
	rows, queryErr := store.Query(ctx, calibrationConformalWindowQuery,
		artifactKind, targetSurface, scopeKind, scopeID,
		int64(pairLimit(windowRows, ceiling)))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	judgements := make([]db2contract.CalibrationConformalWindowRow, 0, 32)
	for rows.Next() {
		var confidence float64
		var verdict string
		if scanErr := rows.Scan(&confidence, &verdict); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		judgements = append(judgements, db2contract.CalibrationConformalWindowRow{
			AppliedConfidence: confidence,
			Verdict:           verdict,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr :=
		db2contract.EncodeCalibrationConformalWindowReply(judgements)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The conflict target is the subject and the feature set version together, not
// the subject alone: features computed under two versions are two rows, because
// a model trained on one cannot read the other. Recomputing under the same
// version replaces.
//
// The scope moves on conflict, which is worth noticing -- a subject whose scope
// changed keeps one row and the new scope, rather than accumulating a row per
// scope it has ever had.
const featureRowUpsertQuery = `INSERT INTO feature_rows
 (subject_id, subject_kind, scope_kind, scope_id, feature_set_version,
  features, computed_at)
 VALUES ($1, $2, $3, $4, $5, $6::jsonb,
   CASE WHEN $7 = '' THEN pg_now_text() ELSE $7 END)
 ON CONFLICT (subject_id, subject_kind, feature_set_version) DO UPDATE SET
  features = EXCLUDED.features, computed_at = EXCLUDED.computed_at,
  scope_kind = EXCLUDED.scope_kind, scope_id = EXCLUDED.scope_id`

// featureRowUpsert stores the features computed for one subject.
//
// The computed-at time is the caller's when it gives one, because features are
// often computed from a snapshot rather than from now -- and a backfill stamped
// with the time it ran would look like fresh evidence about an old state.
func featureRowUpsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	subjectID, subjectKind, scopeKind, scopeID, version, features, computedAt, err :=
		db2contract.DecodeFeatureRowUpsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, featureRowUpsertQuery, subjectID, subjectKind,
		scopeKind, scopeID, version, features, computedAt)
	return acknowledgement(execErr == nil,
		db2contract.EncodeFeatureRowUpsertReply)
}

// corroboration_count starts at one: the proposal is its own first piece of
// corroboration, and starting at zero would make a single-source proposal look
// like one nothing supports.
const learningProposalInsertQuery = `INSERT INTO learning_proposals
 (signal_id, sink, state, target_key, target_memory_id, action_json,
  evidence_refs, corroboration_count, expires_at, created_at, updated_at)
 VALUES ($1, $2, 'pending', $3, $4, $5,
  CASE WHEN $6 = '' THEN '[]' ELSE $6 END, 1, $7, pg_now_text(), pg_now_text())
 RETURNING id`

// learningProposalInsert records something learned that has not been acted on.
//
// Pending on arrival: a proposal is a suggestion, and the whole point of the
// table is that something else decides. The identifier comes back so the caller
// can follow what it proposed.
//
// The evidence references default to an empty JSON array rather than an empty
// string, which the C does too -- the column holds text that readers parse as
// JSON, and an empty string is not an empty list. Both JSON columns here are
// TEXT rather than JSONB, so neither is cast: casting would reformat what the
// caller wrote and store something it did not send.
func learningProposalInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	signalID, sink, targetKey, targetMemoryID, action, evidence, expiresAt, err :=
		db2contract.DecodeLearningProposalInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var proposalID int64
	if scanErr := store.QueryRow(ctx, learningProposalInsertQuery,
		int64(signalID), sink, targetKey, int64(targetMemoryID), action,
		evidence, expiresAt).Scan(&proposalID); scanErr != nil {
		proposalID = 0
	}
	if proposalID < 0 {
		proposalID = 0
	}
	reply, encodeErr := db2contract.EncodeLearningProposalInsertReply(
		clampToU32(proposalID))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
