package db2

import (
	"errors"
	"github.com/jackc/pgx/v5"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestEvidenceWithTheSameHashCollapses(t *testing.T) {
	// Re-uploading a session or a feedback batch is the normal case, and
	// duplicating it would weight the same observation twice everywhere
	// evidence is counted.
	store := &fakeStore{row: &fakeRow{values: []any{"artifact-existing"}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactWriteEvidenceRequest(
		"session_evidence", "project", "aimee", "jbailes", "abc123", `{"turns":4}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactWriteEvidence), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	artifactID, decodeErr := db2contract.DecodeArtifactWriteEvidenceReply(body)
	if decodeErr != nil || artifactID != "artifact-existing" {
		t.Fatalf("artifact = %q, want the row already there", artifactID)
	}
	if store.execCalls != 0 {
		t.Errorf("a duplicate row was written anyway")
	}
	if !strings.Contains(store.lastSQL, "source_bundle_hash = $2") {
		t.Errorf("the hash is no longer what identifies a repeat: %q", store.lastSQL)
	}
}

func TestUnhashedEvidenceIsNeverCollapsed(t *testing.T) {
	// An unhashed batch cannot be compared against anything, so treating every
	// unhashed batch as the same one would collapse them all onto the first.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeArtifactWriteEvidenceRequest(
		"session_evidence", "", "", "", "", `{"turns":4}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageArtifactWriteEvidence), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	artifactID, decodeErr := db2contract.DecodeArtifactWriteEvidenceReply(body)
	if decodeErr != nil || artifactID == "" {
		t.Fatalf("artifact = %q", artifactID)
	}
	if len(store.sqlLog) != 1 || !strings.Contains(store.sqlLog[0], "INSERT INTO artifacts") {
		t.Fatalf("statements = %v, want only the insert", store.sqlLog)
	}
	// Evidence is proposed rather than committed: it is an observation, and
	// what it is worth is decided by whatever reads it.
	if !strings.Contains(store.lastSQL, "'proposed'") {
		t.Errorf("evidence now arrives already committed: %q", store.lastSQL)
	}
	if store.lastArgs[2] != "user" {
		t.Errorf("scope kind = %v, want the default", store.lastArgs[2])
	}
}

func TestBanditDecisionBindsABoolean(t *testing.T) {
	// The column is BOOLEAN. The C binds a double, which works only because
	// libpq renders it as text; pgx sends the parameter typed, so a float would
	// be refused outright.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditDecisionInsertRequest(
		"decision-1", "recall", "arm-a", "ctx", 0.25, 1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditDecisionInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeBanditDecisionInsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if exploration, ok := store.lastArgs[5].(bool); !ok || !exploration {
		t.Fatalf("is_exploration = %#v, want a boolean true", store.lastArgs[5])
	}
	// The propensity is what makes the log usable afterwards: an outcome from
	// an arm chosen a tenth of the time counts differently from one chosen
	// almost always.
	if store.lastArgs[4] != 0.25 {
		t.Errorf("propensity = %v", store.lastArgs[4])
	}
	// A decision is a thing that happened, and the second telling of it is not
	// new information.
	if !strings.Contains(store.lastSQL, "ON CONFLICT (id) DO NOTHING") {
		t.Errorf("a retry would rewrite the decision: %q", store.lastSQL)
	}
}

func TestCalibrationProfileIsFindableAsItLands(t *testing.T) {
	// The three stamped columns are how a profile is found again, so the C's
	// write-then-stamp leaves a window in which the profile exists and nothing
	// can find it.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationProfileWriteRequest(
		"recall", "synthesis", "", "", "", `{"bins":10}`)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCalibrationProfileWrite), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	artifactID, decodeErr := db2contract.DecodeCalibrationProfileWriteReply(body)
	if decodeErr != nil || artifactID == "" {
		t.Fatalf("artifact = %q", artifactID)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d, want one insert", store.execCalls)
	}
	// model_version carries the artifact kind, prompt_version the feature set
	// version, target_surface the surface -- all three named for something
	// else, as the C has them.
	if store.lastArgs[3] != "synthesis" || store.lastArgs[4] != "v1" ||
		store.lastArgs[5] != "recall" {
		t.Fatalf("stamps = %v", store.lastArgs)
	}
	if store.lastArgs[1] != "global" {
		t.Errorf("scope kind = %v, want the default", store.lastArgs[1])
	}
	if !strings.Contains(store.lastSQL, "$7::jsonb") {
		t.Errorf("the payload is not cast for a JSONB column: %q", store.lastSQL)
	}
}

func TestConformalWindowCountsOnlyDecidedJudgements(t *testing.T) {
	// An abstention is not evidence either way, and including it would drag the
	// calibration toward the middle.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{0.9, "accepted"}, {0.4, "rejected"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCalibrationConformalWindowRequest(
		"recall", "synthesis", "project", "aimee", 128)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageCalibrationConformalWindow), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	judgements, decodeErr :=
		db2contract.DecodeCalibrationConformalWindowReply(body)
	if decodeErr != nil || len(judgements) != 2 ||
		judgements[0].AppliedConfidence != 0.9 || judgements[1].Verdict != "rejected" {
		t.Fatalf("judgements = %+v", judgements)
	}
	if !strings.Contains(store.lastSQL, "ae.verdict IN ('accepted', 'rejected')") {
		t.Errorf("abstentions would calibrate: %q", store.lastSQL)
	}
	// Newest first: calibration is about how the current model is doing, and a
	// window reaching back to the oldest judgements measures a model that has
	// since been replaced.
	if !strings.Contains(store.lastSQL, "ORDER BY ae.applied_at DESC") {
		t.Errorf("the window is no longer recent: %q", store.lastSQL)
	}
}

func TestConformalScopeIsOneStatementNotThree(t *testing.T) {
	// An empty scope kind admits every scope, and an empty scope id admits
	// every identifier within the kind. The C builds three statement texts to
	// say that; the predicates say it once.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	for _, probe := range []struct{ kind, id string }{
		{"", ""}, {"project", ""}, {"project", "aimee"},
	} {
		request, err := db2contract.EncodeCalibrationConformalWindowRequest(
			"recall", "synthesis", probe.kind, probe.id, 128)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		if _, status := handler(
			invocation(db2contract.StageCalibrationConformalWindow), request); status !=
			bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		if !strings.Contains(store.lastSQL, "($3 = '' OR ae.scope_kind = $3)") ||
			!strings.Contains(store.lastSQL, "($3 = '' OR $4 = '' OR ae.scope_id = $4)") {
			t.Fatalf("the scope predicates changed: %q", store.lastSQL)
		}
		if store.lastArgs[2] != probe.kind || store.lastArgs[3] != probe.id {
			t.Fatalf("args = %v for %+v", store.lastArgs, probe)
		}
	}
}

func TestFeatureRowsAreKeyedByVersionAsWellAsSubject(t *testing.T) {
	// Features computed under two versions are two rows, because a model
	// trained on one cannot read the other. Recomputing under the same version
	// replaces.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeFeatureRowUpsertRequest(
		"memory:4", "memory", "project", "aimee", "v3", `{"uses":9}`, "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageFeatureRowUpsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeFeatureRowUpsertReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if !strings.Contains(store.lastSQL,
		"ON CONFLICT (subject_id, subject_kind, feature_set_version)") {
		t.Errorf("two versions would collapse onto one row: %q", store.lastSQL)
	}
	// An empty computed-at falls back to now; a given one is kept, because
	// features are often computed from a snapshot rather than from now.
	if !strings.Contains(store.lastSQL, "CASE WHEN $7 = '' THEN pg_now_text() ELSE $7 END") {
		t.Errorf("a backfill would be stamped as fresh: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "$6::jsonb") {
		t.Errorf("the features are not cast for a JSONB column: %q", store.lastSQL)
	}
}

func TestProposalsStartPendingAndCorroboratedOnce(t *testing.T) {
	// A proposal is a suggestion, and the whole point of the table is that
	// something else decides. Starting the corroboration count at zero would
	// make a single-source proposal look like one nothing supports.
	store := &fakeStore{row: &fakeRow{values: []any{int64(31)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalInsertRequest(
		7, "rules", "build-state", 4, `{"op":"reinforce"}`, "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLearningProposalInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	proposalID, decodeErr := db2contract.DecodeLearningProposalInsertReply(body)
	if decodeErr != nil || proposalID != 31 {
		t.Fatalf("proposal = %d", proposalID)
	}
	if !strings.Contains(store.lastSQL, "'pending'") {
		t.Errorf("a proposal now arrives already decided: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "1, $7, pg_now_text(), pg_now_text())") {
		t.Errorf("the corroboration count changed: %q", store.lastSQL)
	}
	// The two JSON columns are TEXT rather than JSONB, so neither is cast --
	// casting would reformat what the caller wrote.
	if strings.Contains(store.lastSQL, "$5::jsonb") {
		t.Errorf("a TEXT column is being cast: %q", store.lastSQL)
	}
	// An empty evidence list is an empty JSON array, not an empty string: the
	// column holds text that readers parse as JSON.
	if !strings.Contains(store.lastSQL, "CASE WHEN $6 = '' THEN '[]' ELSE $6 END") {
		t.Errorf("an absent evidence list would not parse: %q", store.lastSQL)
	}
}

func TestAProposalThatWasNotCreatedAnswersZero(t *testing.T) {
	// The guard refused it: no row came back, and zero says so.
	store := &fakeStore{row: &fakeRow{err: pgx.ErrNoRows}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalInsertRequest(
		7, "rules", "", 0, "{}", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageLearningProposalInsert), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	proposalID, decodeErr := db2contract.DecodeLearningProposalInsertReply(body)
	if decodeErr != nil || proposalID != 0 {
		t.Fatalf("proposal = %d, want 0", proposalID)
	}
}

func TestAProposalTheDatabaseCouldNotAnswerIsNotZero(t *testing.T) {
	// This test previously asserted the opposite, under the name
	// TestProposalFailureAnswersZero: a lost connection answered zero, and the
	// caller could not tell "the guard refused your proposal" from "the database
	// did not answer". The first is a decision about the request; the second is
	// an outage, and a caller that reads it as a refusal stops retrying and
	// records a proposal that was never considered.
	store := &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeLearningProposalInsertRequest(
		7, "rules", "", 0, "{}", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageLearningProposalInsert),
		request); status == bus.ModuleStatusOK {
		t.Fatal("a failed scan answered OK; zero is indistinguishable from a refusal")
	}
}
