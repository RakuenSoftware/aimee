package db2

import (
	"context"
	"encoding/json"
	"math"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageAntiPatternInsert,
		db2contract.OperationAntiPatternInsert, antiPatternInsert)
	Register(db2contract.StageWorkflowPatternInsert,
		db2contract.OperationWorkflowPatternInsert, workflowPatternInsert)
	Register(db2contract.StageFeedbackRecord,
		db2contract.OperationFeedbackRecord, feedbackRecord)
	Register(db2contract.StageDemotionScore,
		db2contract.OperationDemotionScore, demotionScore)
}

// The two pattern tables take the same five columns and answer the same way.
// They are separate tables rather than one with a polarity column, which is a
// schema decision this port inherits: a pattern and an anti-pattern are matched
// by different passes and never mixed.
const (
	antiPatternInsertQuery = `INSERT INTO anti_patterns
 (pattern, description, source, source_ref, confidence)
 VALUES ($1, $2, $3, $4, $5) RETURNING id`
	workflowPatternInsertQuery = `INSERT INTO workflow_patterns
 (pattern, description, source, source_ref, confidence)
 VALUES ($1, $2, $3, $4, $5) RETURNING id`
)

// insertPattern runs one of the two pattern inserts and answers the identifier.
//
// Zero means nothing was written. No row carries it, so a caller cannot mistake
// it for a pattern it can cite.
func insertPattern(ctx context.Context, store Store, query, pattern, description,
	source, sourceRef string, confidence float64,
) uint64 {
	var patternID int64
	if err := store.QueryRow(ctx, query,
		pattern, description, source, sourceRef, confidence).Scan(&patternID); err != nil {
		return 0
	}
	if patternID < 0 {
		return 0
	}
	return uint64(patternID)
}

// antiPatternInsert records something not to do.
func antiPatternInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pattern, description, source, sourceRef, confidence, err :=
		db2contract.DecodeAntiPatternInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	reply, encodeErr := db2contract.EncodeAntiPatternInsertReply(
		insertPattern(ctx, store, antiPatternInsertQuery,
			pattern, description, source, sourceRef, confidence))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// workflowPatternInsert records a way of working that has gone well.
func workflowPatternInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	pattern, description, source, sourceRef, confidence, err :=
		db2contract.DecodeWorkflowPatternInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	reply, encodeErr := db2contract.EncodeWorkflowPatternInsertReply(
		insertPattern(ctx, store, workflowPatternInsertQuery,
			pattern, description, source, sourceRef, confidence))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Titles are matched case-insensitively, and the subselect is what keeps the
// update to one row: nothing constrains rules to unique titles, so a bare
// WHERE LOWER(title) = LOWER($1) would reinforce every rule sharing a title
// where the C reinforces the one it happened to read.
//
// LEAST caps the reinforced weight at a hundred. Reinforcement is meant to
// saturate -- a rule stated five times is not five times as binding as one
// stated once, and without the cap repeated feedback would run the weight away
// from the scale everything else reads it on.
const feedbackReinforceQuery = `UPDATE rules
 SET weight = LEAST(weight + 50, 100), description = $2,
 updated_at = pg_now_text(), last_reinforced_at = pg_now_text()
 WHERE id = (SELECT id FROM rules WHERE LOWER(title) = LOWER($1) LIMIT 1)
 RETURNING id`

const feedbackInsertQuery = `INSERT INTO rules
 (polarity, title, description, weight, domain, created_at, updated_at)
 VALUES ($1, $2, $3, $4, '', pg_now_text(), pg_now_text())
 RETURNING id`

// feedbackNewRuleWeightCap is the ceiling a new rule's weight is clamped to.
const feedbackNewRuleWeightCap = 100

// feedbackRecord turns a piece of feedback into a rule, or reinforces the rule
// it repeats.
//
// Reinforce-or-insert in one transaction, where the C reads and then writes
// without one. That read-then-write is a real window: two pieces of feedback
// carrying the same title can both find nothing and both insert, leaving two
// rules that will never merge because the next reinforcement picks one of them.
//
// The description is overwritten on reinforcement, empty included. The C falls
// back to the existing description for a NULL one, but the adapter decodes into
// a buffer so the pointer is never NULL -- through the module, feedback with no
// description erases the description the rule had.
//
// A new rule's weight is the one the caller gave, capped at a hundred. Both
// halves of that are already decided elsewhere: the envelope bounds the
// override at a hundred, so the cap here never fires, and the override crosses
// the wire unsigned, so the C's fall back to fifty for a negative one is
// unreachable. What that leaves is worth knowing -- feedback asking for weight
// zero gets a rule of weight zero rather than a middling one, and zero is what
// an unset field encodes as.
func feedbackRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	polarity, title, description, weightOverride, err :=
		db2contract.DecodeFeedbackRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	weight := int64(weightOverride)
	if weight > feedbackNewRuleWeightCap {
		weight = feedbackNewRuleWeightCap
	}

	var ruleID int64
	reinforced := false
	txErr := store.InTx(ctx, func(tx Store) error {
		scanErr := tx.QueryRow(ctx, feedbackReinforceQuery, title, description).Scan(&ruleID)
		if scanErr == nil {
			reinforced = true
			return nil
		}
		return tx.QueryRow(ctx, feedbackInsertQuery,
			polarity, title, description, weight).Scan(&ruleID)
	})
	if txErr != nil {
		ruleID, reinforced = 0, false
	}
	if ruleID < 0 {
		ruleID = 0
	}
	reinforcedFlag := uint32(0)
	if reinforced {
		reinforcedFlag = 1
	}
	reply, encodeErr := db2contract.EncodeFeedbackRecordReply(
		clampToU32(ruleID), reinforcedFlag)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The window and the stamp in one statement.
//
// Scoring consumes retrieval-attribution artifacts, and the C stamps each one
// as read after the select finishes so temporal maintenance sees the evidence
// as active. The data-modifying CTE does the same thing in one round trip and
// closes the gap where the C can score a window and then fail to stamp it.
//
// The age is computed by the database rather than in Go. The column holds text
// in either of two spellings the tree has written over time, and PostgreSQL
// parses both -- reproducing that by hand is the kind of thing that silently
// returns the epoch and makes every attribution look ancient, which is a
// failure this file's C neighbours carry a comment about.
const demotionScoreQuery = `WITH attributions AS (
 SELECT id, payload, GREATEST(
   EXTRACT(EPOCH FROM ((CURRENT_TIMESTAMP AT TIME ZONE 'UTC') - created_at::timestamp))
   / 86400.0, 0) AS age_days
 FROM artifacts
 WHERE kind = 'retrieval_attribution' AND scope_id = $1
 ORDER BY created_at DESC
 LIMIT $2
), touched AS (
 UPDATE artifacts SET last_accessed_at = CURRENT_TIMESTAMP
 FROM attributions WHERE artifacts.id = attributions.id
)
SELECT payload, age_days FROM attributions`

// The C's defaults for a caller that asked for nothing sensible.
const (
	demotionScoreDefaultWindow   = 64
	demotionScoreDefaultHalfLife = 30.0
	demotionScoreDefaultMinimum  = 5
)

// verdictSign is what a verdict contributes to the score.
//
// Accepted counts for, the three ways of being wrong count against, and
// anything else counts for nothing at all. An unrecognised verdict scores zero
// rather than being skipped, which matters: it still counts toward the minimum
// sample size, so a window full of verdicts nobody understands produces a
// confident zero rather than no answer.
func verdictSign(verdict string) float64 {
	switch verdict {
	case "accepted":
		return 1
	case "corrected", "contradicted", "rolled_back":
		return -1
	default:
		return 0
	}
}

// demotionScore weighs what a surfaced row has been worth lately.
//
// Recent evidence counts for more, halving every half_life_days, so a row that
// was useful a year ago and useless since scores as useless. A negative score
// is the case for demoting it.
//
// Below the minimum sample size there is no answer rather than a score near
// zero. Two attributions cannot distinguish a row nobody wants from a row
// nobody has seen, and reporting a small negative number would let the second
// be demoted as though it were the first.
func demotionScore(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	rowID, windowSize, halfLifeDays, minimumSamples, err :=
		db2contract.DecodeDemotionScoreRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if windowSize == 0 {
		windowSize = demotionScoreDefaultWindow
	}
	if halfLifeDays <= 0 {
		halfLifeDays = demotionScoreDefaultHalfLife
	}
	if minimumSamples == 0 {
		minimumSamples = demotionScoreDefaultMinimum
	}

	rows, queryErr := store.Query(ctx, demotionScoreQuery,
		strconv.FormatUint(rowID, 10), int64(windowSize))
	if queryErr != nil {
		return unscored()
	}
	defer rows.Close()

	score := 0.0
	valid := uint32(0)
	for rows.Next() {
		var payload string
		var ageDays float64
		if scanErr := rows.Scan(&payload, &ageDays); scanErr != nil {
			return unscored()
		}
		// A payload that is not an object, or carries no verdict, or carries
		// one that is not a string, contributes nothing and is not counted --
		// the same three refusals the C's cJSON checks make.
		var decoded map[string]any
		if json.Unmarshal([]byte(payload), &decoded) != nil {
			continue
		}
		verdict, ok := decoded["verdict"].(string)
		if !ok {
			continue
		}
		weight := 1.0
		if given, isNumber := decoded["weight"].(float64); isNumber {
			weight = given
		}
		score += verdictSign(verdict) * weight *
			math.Exp(-math.Ln2*ageDays/halfLifeDays)
		valid++
	}
	if rows.Err() != nil {
		return unscored()
	}
	if valid < minimumSamples {
		return unscored()
	}

	reply, encodeErr := db2contract.EncodeDemotionScoreReply(1, score)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// unscored is the reply for a row that cannot be scored: the flag clear and the
// score zero.
//
// Zero with the flag clear is not "neutral evidence" -- it is no evidence, and
// a caller ignoring the flag would demote a row it has never seen used.
func unscored() ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeDemotionScoreReply(0, 0)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
