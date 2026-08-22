package db2

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func TestAggregateOverNoRowsIsEmptyNotAFailure(t *testing.T) {
	// MAX() over nothing is NULL even when the column it reads is NOT NULL, so
	// these scan through a pointer. Scanning directly would fail the read and
	// report an outage for an install that has simply never scanned.
	for _, testCase := range []struct {
		name  string
		stage uint32
		build func() ([]byte, error)
		read  func(t *testing.T, body []byte) string
	}{
		{
			"memory_last_retro_scan",
			db2contract.StageMemoryLastRetroScan,
			db2contract.EncodeMemoryLastRetroScanRequest,
			func(t *testing.T, body []byte) string {
				value, err := db2contract.DecodeMemoryLastRetroScanReply(body)
				if err != nil {
					t.Fatalf("decode: %v", err)
				}
				return value
			},
		},
		{
			"project_last_scan",
			db2contract.StageProjectLastScan,
			db2contract.EncodeProjectLastScanRequest,
			func(t *testing.T, body []byte) string {
				value, err := db2contract.DecodeProjectLastScanReply(body)
				if err != nil {
					t.Fatalf("decode: %v", err)
				}
				return value
			},
		},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{row: &fakeRow{values: []any{(*string)(nil)}}}
			handler := NewDispatchHandler(store)
			request, err := testCase.build()
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v -- a NULL aggregate read as a failure", status)
			}
			if got := testCase.read(t, body); got != "" {
				t.Fatalf("value = %q, want empty", got)
			}
		})
	}
}

func TestProjectCurrentGenerationAbsenceIsZero(t *testing.T) {
	// A project that is not current, or not there, has no generation. Zero is
	// the answer rather than an error: a caller asking which generation to read
	// is entitled to be told there is none.
	handler := NewDispatchHandler(&fakeStore{})
	request, err := db2contract.EncodeProjectCurrentGenerationRequest("absent")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectCurrentGeneration), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	generation, decodeErr := db2contract.DecodeProjectCurrentGenerationReply(body)
	if decodeErr != nil || generation != 0 {
		t.Fatalf("generation = %d, err = %v", generation, decodeErr)
	}
}

func TestProjectCurrentGenerationReadsTheValue(t *testing.T) {
	store := &fakeStore{row: &fakeRow{values: []any{int64(7)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectCurrentGenerationRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectCurrentGeneration), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	generation, decodeErr := db2contract.DecodeProjectCurrentGenerationReply(body)
	if decodeErr != nil || generation != 7 {
		t.Fatalf("generation = %d", generation)
	}
	if len(store.lastArgs) != 1 || store.lastArgs[0] != "replay-project" {
		t.Fatalf("args = %v", store.lastArgs)
	}
}

func TestBanditDecisionPointsEscapesWhatCDidNot(t *testing.T) {
	// The C builder formats each element as "%s" with no escaping, so a value
	// carrying a quote or a backslash produced a document its own caller could
	// not parse. Encoding properly is a fix, not a behaviour change: there is no
	// reading under which the broken output was correct.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{`plain`},
		{`has "quotes"`},
		{`has\backslash`},
		{``}, // skipped, as the C builder skips it
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditDecisionPointsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditDecisionPoints), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	encoded, decodeErr := db2contract.DecodeBanditDecisionPointsReply(body)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	var points []string
	if err := json.Unmarshal([]byte(encoded), &points); err != nil {
		t.Fatalf("the reply is not parseable JSON: %v (%q)", err, encoded)
	}
	if len(points) != 3 || points[1] != `has "quotes"` || points[2] != `has\backslash` {
		t.Fatalf("points = %q", points)
	}
}

func TestBanditDecisionPointsEmptyIsAnEmptyArray(t *testing.T) {
	// "[]" rather than "", so a caller can parse the reply unconditionally.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditDecisionPointsRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditDecisionPoints), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	encoded, decodeErr := db2contract.DecodeBanditDecisionPointsReply(body)
	if decodeErr != nil || encoded != "[]" {
		t.Fatalf("encoded = %q, want []", encoded)
	}
}

func TestActiveEmbedderVersionAbsenceIsEmpty(t *testing.T) {
	handler := NewDispatchHandler(&fakeStore{row: &fakeRow{err: pgx.ErrNoRows}})
	request, err := db2contract.EncodeActiveEmbedderVersionRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageActiveEmbedderVersion), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	version, decodeErr := db2contract.DecodeActiveEmbedderVersionReply(body)
	if decodeErr != nil || version != "" {
		t.Fatalf("version = %q", version)
	}
}

func TestProjectFingerprintKeepsTheStatementsCoalesce(t *testing.T) {
	// A project with no files fingerprints as the md5 of the empty string, not
	// as NULL. Moving that out of the statement would change what a fileless
	// project reports.
	store := &fakeStore{row: &fakeRow{values: []any{(*string)(nil)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectFingerprintRequest("replay-project")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageProjectFingerprint), request); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.Contains(store.lastSQL, "coalesce(string_agg") {
		t.Error("the coalesce left the statement")
	}
	if !strings.Contains(store.lastSQL, "ORDER BY f.path") {
		t.Error("the aggregate is unordered, so the fingerprint is not stable")
	}
}
