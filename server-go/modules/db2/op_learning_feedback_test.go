package db2

import (
	"errors"
	"math"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestPatternInsertsAnswerWhatTheyWrote(t *testing.T) {
	// The identifier is the whole reply, so a caller can cite the pattern it
	// just recorded. Zero means nothing was written and no row carries it.
	for _, probe := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		read  func([]byte) (uint64, error)
		table string
	}{
		{
			name:  "anti",
			stage: db2contract.StageAntiPatternInsert,
			build: func() ([]byte, error) {
				return db2contract.EncodeAntiPatternInsertRequest(
					"retry without backoff", "hammers the service", "review", "pr/1", 0.8)
			},
			read:  db2contract.DecodeAntiPatternInsertReply,
			table: "anti_patterns",
		},
		{
			name:  "workflow",
			stage: db2contract.StageWorkflowPatternInsert,
			build: func() ([]byte, error) {
				return db2contract.EncodeWorkflowPatternInsertRequest(
					"probe then commit", "keeps batches reviewable", "review", "pr/1", 0.8)
			},
			read:  db2contract.DecodeWorkflowPatternInsertReply,
			table: "workflow_patterns",
		},
	} {
		t.Run(probe.name, func(t *testing.T) {
			store := &fakeStore{row: &fakeRow{values: []any{int64(12)}}}
			handler := NewDispatchHandler(store)
			request, err := probe.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(probe.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			patternID, decodeErr := probe.read(body)
			if decodeErr != nil || patternID != 12 {
				t.Fatalf("pattern id = %d", patternID)
			}
			// Two tables rather than one with a polarity column: a pattern and
			// an anti-pattern are matched by different passes and never mixed.
			if !strings.Contains(store.lastSQL, "INTO "+probe.table) {
				t.Errorf("wrote to the wrong table: %q", store.lastSQL)
			}
		})
	}
}

func TestPatternInsertFailureAnswersZero(t *testing.T) {
	store := &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAntiPatternInsertRequest(
		"retry without backoff", "", "", "", 0.8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAntiPatternInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	patternID, decodeErr := db2contract.DecodeAntiPatternInsertReply(body)
	if decodeErr != nil || patternID != 0 {
		t.Fatalf("pattern id = %d, want 0", patternID)
	}
}

func TestFeedbackReinforcesRatherThanDuplicating(t *testing.T) {
	// Repeating feedback should strengthen the rule it repeats. The reply says
	// which happened, and a caller showing "recorded" for a reinforcement would
	// tell someone their point was new when it was not.
	store := &fakeStore{rowQueue: []*fakeRow{{values: []any{int64(4)}}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFeedbackRecordRequest(
		"positive", "Write it down", "still true", 60)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFeedbackRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	ruleID, reinforced, decodeErr := db2contract.DecodeFeedbackRecordReply(body)
	if decodeErr != nil || ruleID != 4 || reinforced != 1 {
		t.Fatalf("rule = %d, reinforced = %d", ruleID, reinforced)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	// One statement, not two: the insert is never reached when the update
	// found a rule.
	if len(store.sqlLog) != 1 {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if !strings.Contains(store.sqlLog[0], "LEAST(weight + 50, 100)") {
		t.Errorf("reinforcement no longer saturates: %q", store.sqlLog[0])
	}
	// Nothing constrains rules to unique titles, so the subselect is what keeps
	// the update to one row where a bare LOWER(title) match would reinforce
	// every rule sharing a title.
	if !strings.Contains(store.sqlLog[0], "WHERE id = (SELECT id FROM rules") {
		t.Errorf("the update is no longer bounded to one rule: %q", store.sqlLog[0])
	}
}

func TestFeedbackInsertsWhenNothingMatches(t *testing.T) {
	// The reinforce statement returning no row is the signal to insert, and
	// both live in one transaction: without it two pieces of feedback under the
	// same title can both find nothing and both insert, leaving two rules that
	// will never merge.
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},
		{values: []any{int64(9)}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFeedbackRecordRequest(
		"negative", "Do not skip the gate", "", 70)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFeedbackRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	ruleID, reinforced, decodeErr := db2contract.DecodeFeedbackRecordReply(body)
	if decodeErr != nil || ruleID != 9 || reinforced != 0 {
		t.Fatalf("rule = %d, reinforced = %d", ruleID, reinforced)
	}
	if len(store.sqlLog) != 2 || !strings.Contains(store.sqlLog[1], "INSERT INTO rules") {
		t.Fatalf("statements = %v", store.sqlLog)
	}
	if store.argsLog[1][3] != int64(70) {
		t.Errorf("weight = %v", store.argsLog[1][3])
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
}

func TestFeedbackWeightCannotExceedTheScale(t *testing.T) {
	// The envelope bounds the override at a hundred, so the cap inside the
	// operation is unreachable through the wire. It is kept because the C keeps
	// it and because the bound belonging to the envelope is a fact about
	// today's schema rather than about this operation.
	if _, err := db2contract.EncodeFeedbackRecordRequest(
		"positive", "Everything matters", "", 101); err == nil {
		t.Error("a weight above the scale encoded")
	}
	store := &fakeStore{rowQueue: []*fakeRow{
		{err: pgx.ErrNoRows},
		{values: []any{int64(9)}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFeedbackRecordRequest(
		"positive", "Everything matters", "", 100)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageFeedbackRecord), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.argsLog[1][3] != int64(100) {
		t.Errorf("weight = %v", store.argsLog[1][3])
	}
}

func TestDemotionScoreWeighsRecentEvidenceMore(t *testing.T) {
	// Halving every half_life_days, so a row that was useful a year ago and
	// useless since scores as useless. Two attributions of equal weight and
	// opposite verdicts do not cancel unless they are the same age.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{`{"verdict":"accepted","weight":1.0}`, 0.0},
		{`{"verdict":"contradicted","weight":1.0}`, 30.0},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionScoreRequest(4321, 64, 30.0, 2)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionScore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	valid, score, decodeErr := db2contract.DecodeDemotionScoreReply(body)
	if decodeErr != nil || valid != 1 {
		t.Fatalf("valid = %d", valid)
	}
	// The fresh acceptance counts fully; the contradiction is exactly one
	// half-life old and counts half.
	if math.Abs(score-0.5) > 1e-9 {
		t.Fatalf("score = %v, want 0.5", score)
	}
	// The window is stamped as read in the same statement: scoring consumes
	// the evidence, and temporal maintenance has to see it as active.
	if !strings.Contains(store.lastSQL, "UPDATE artifacts SET last_accessed_at") {
		t.Errorf("the attributions are no longer stamped: %q", store.lastSQL)
	}
	// The row identifier is matched as text, because scope_id is a text column
	// carrying the surfaced row.
	if store.lastArgs[0] != "4321" {
		t.Errorf("scope id = %v", store.lastArgs[0])
	}
}

func TestDemotionScoreRefusesTooSmallASample(t *testing.T) {
	// Two attributions cannot tell a row nobody wants from a row nobody has
	// seen. Reporting a small negative number would let the second be demoted
	// as though it were the first.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{`{"verdict":"contradicted","weight":1.0}`, 0.0},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionScoreRequest(4321, 64, 30.0, 5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionScore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	valid, score, decodeErr := db2contract.DecodeDemotionScoreReply(body)
	if decodeErr != nil || valid != 0 || score != 0 {
		t.Fatalf("valid = %d, score = %v", valid, score)
	}
}

func TestDemotionScoreIgnoresPayloadsItCannotRead(t *testing.T) {
	// A payload that is not JSON, or carries no verdict, or carries one that is
	// not a string, contributes nothing and is not counted -- the same three
	// refusals the C's JSON checks make. An unrecognised verdict is different:
	// it scores zero but still counts, so a window of verdicts nobody
	// understands answers a confident zero rather than no answer.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{`not json at all`, 0.0},
		{`{"weight":1.0}`, 0.0},
		{`{"verdict":42}`, 0.0},
		{`{"verdict":"something_new"}`, 0.0},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionScoreRequest(4321, 64, 30.0, 1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionScore), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	valid, score, decodeErr := db2contract.DecodeDemotionScoreReply(body)
	if decodeErr != nil || valid != 1 || score != 0 {
		t.Fatalf("valid = %d, score = %v", valid, score)
	}
}

func TestDemotionScoreFallsBackToTheCDefaults(t *testing.T) {
	// Zero is what an unset field encodes as, and a window of zero rows would
	// answer "no evidence" for every row in the database.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionScoreRequest(4321, 0, 0, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageDemotionScore), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[1] != int64(demotionScoreDefaultWindow) {
		t.Errorf("window = %v, want the default", store.lastArgs[1])
	}
}
