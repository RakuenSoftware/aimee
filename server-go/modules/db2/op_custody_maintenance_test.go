package db2

import (
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestLintRunsAllThreeChecksAndLabelsEach(t *testing.T) {
	// Three statements, one reply. The issue type is what a caller routes on,
	// so a row labelled with the wrong one sends someone to fix the wrong
	// thing.
	store := &fakeStore{rowsQueue: []*fakeRows{
		{values: [][]any{{int64(4), "build-state"}}},
		{values: [][]any{{"deploy-steps", int64(5)}}},
		{values: [][]any{{int64(9), "release-notes", "old-target", 0.05}}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryLintRequest()
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryLint), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	issues, decodeErr := db2contract.DecodeMemoryLintReply(body)
	if decodeErr != nil || len(issues) != 3 {
		t.Fatalf("issues = %+v", issues)
	}
	if issues[0].IssueType != "orphan" || issues[0].LintMemoryID != 4 ||
		issues[0].MemoryKey != "build-state" {
		t.Fatalf("orphan = %+v", issues[0])
	}
	// A concept gap carries no memory identifier: the issue is a key with
	// several memories and no concept among them, so there is no single memory
	// to point at. Zero means "not a memory", which no row has.
	if issues[1].IssueType != "concept_gap" || issues[1].LintMemoryID != 0 {
		t.Fatalf("gap = %+v", issues[1])
	}
	if !strings.Contains(issues[1].IssueMessage, "appears 5 times") {
		t.Errorf("gap message = %q", issues[1].IssueMessage)
	}
	if issues[2].IssueType != "stale_ref" || issues[2].LintMemoryID != 9 {
		t.Fatalf("stale = %+v", issues[2])
	}
	// Two decimal places, as the C formats it: more digits of a confidence
	// nobody set precisely would be false precision.
	if !strings.Contains(issues[2].IssueMessage, "confidence=0.05") ||
		!strings.Contains(issues[2].IssueMessage, "'old-target'") {
		t.Errorf("stale message = %q", issues[2].IssueMessage)
	}
}

func TestLintOrphansAreLinkedNeitherWay(t *testing.T) {
	// A memory that only ever appears as a link target is not an orphan, and
	// checking one direction would report half the graph.
	for _, clause := range []string{
		"id NOT IN (SELECT source_id FROM memory_links)",
		"id NOT IN (SELECT target_id FROM memory_links)",
	} {
		if !strings.Contains(memoryLintOrphansQuery, clause) {
			t.Errorf("missing %s", clause)
		}
	}
	// The correlated NOT EXISTS is what makes a gap a gap: a key that already
	// has a concept memory is not one however many other memories share it.
	if !strings.Contains(memoryLintConceptGapsQuery, "m2.key = memories.key") {
		t.Errorf("the concept check is no longer correlated: %q",
			memoryLintConceptGapsQuery)
	}
}

func TestAnchorCoverageRefusesAKeyThatIsNearlyRight(t *testing.T) {
	// A lenient decode would answer "everything is unanchored" for a typo,
	// which reads as a compromised log rather than as a bad request.
	for _, bad := range []string{
		"not-hex-at-all-not-hex-at-allxxxx",
		"abcd",
		strings.Repeat("ab", 20),
	} {
		store := &fakeStore{}
		handler := NewDispatchHandler(store)
		request, err := db2contract.EncodeWitnessCheckpointAnchorCoverageRequest(bad)
		if err != nil {
			// The envelope rejects some of these before the operation sees
			// them, which is the same answer one step earlier.
			continue
		}
		body, status := handler(
			invocation(db2contract.StageWitnessCheckpointAnchorCoverage), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("status = %v", status)
		}
		read, unknown, sample, decodeErr :=
			db2contract.DecodeWitnessCheckpointAnchorCoverageReply(body)
		if decodeErr != nil {
			t.Fatalf("decode reply: %v", decodeErr)
		}
		if read != 0 || unknown != 0 || sample != "" {
			t.Fatalf("%q answered read = %d, unknown = %d", bad, read, unknown)
		}
		if store.lastSQL != "" {
			t.Errorf("%q reached the database: %q", bad, store.lastSQL)
		}
	}
}

func TestAnchorCoverageCountsWhatTheKeyDidNotSign(t *testing.T) {
	// The flag is what separates "nothing unanchored" from "could not tell".
	// A caller ignoring it reads a failed read as a clean bill of health.
	store := &fakeStore{row: &fakeRow{values: []any{int64(3), ptr("00112233")}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeWitnessCheckpointAnchorCoverageRequest(
		strings.Repeat("ab", 16))
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageWitnessCheckpointAnchorCoverage), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	read, unknown, sample, decodeErr :=
		db2contract.DecodeWitnessCheckpointAnchorCoverageReply(body)
	if decodeErr != nil || read != 1 || unknown != 3 || sample != "00112233" {
		t.Fatalf("read = %d, unknown = %d, sample = %q", read, unknown, sample)
	}
	if !strings.Contains(store.lastSQL, "signer_key_id <> $1") {
		t.Errorf("the comparison was inverted: %q", store.lastSQL)
	}
	// Sixteen raw bytes go to the database, not the thirty-two hex characters
	// that came in: the column is a bytea and comparing it against text would
	// match nothing at all.
	raw, ok := store.lastArgs[0].([]byte)
	if !ok || len(raw) != witnessSignerKeyIDLen {
		t.Fatalf("key id = %#v, want sixteen raw bytes", store.lastArgs[0])
	}
}

func TestAnchorCoverageReportsAFailedReadAsUnread(t *testing.T) {
	store := &fakeStore{row: &fakeRow{err: errors.New("connection lost")}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeWitnessCheckpointAnchorCoverageRequest(
		strings.Repeat("ab", 16))
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageWitnessCheckpointAnchorCoverage), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	read, unknown, _, decodeErr :=
		db2contract.DecodeWitnessCheckpointAnchorCoverageReply(body)
	if decodeErr != nil || read != 0 || unknown != 0 {
		t.Fatalf("read = %d, unknown = %d", read, unknown)
	}
}

func TestAuthorityResolveNeedsTheWholeRevocationKey(t *testing.T) {
	// The issuer and serial pair is the revocation key. A certificate can be
	// reissued with a different serial, so matching on the fingerprint alone
	// would resolve a revoked certificate through its replacement.
	store := &fakeStore{row: &fakeRow{values: []any{strings.Repeat("a", 32)}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentAuthorityResolveRequest(
		"fingerprint", "CN=issuer", "01ab")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageEnrollmentAuthorityResolve), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, authorityID, decodeErr :=
		db2contract.DecodeEnrollmentAuthorityResolveReply(body)
	if decodeErr != nil || found != 1 || authorityID != strings.Repeat("a", 32) {
		t.Fatalf("found = %d, authority = %q", found, authorityID)
	}
	for _, clause := range []string{
		"fingerprint = $1", "cert_issuer = $2", "cert_serial_norm = $3",
		"state = 'active'", "revoked_at = ''",
	} {
		if !strings.Contains(store.lastSQL, clause) {
			t.Errorf("missing %s: %q", clause, store.lastSQL)
		}
	}
}

func TestAuthorityOfTheWrongWidthIsNotResolved(t *testing.T) {
	// A row carrying something other than a fixed-width identifier was written
	// by something that did not know the format. Resolving it would hand a
	// caller an authority that cannot be looked up anywhere else.
	store := &fakeStore{row: &fakeRow{values: []any{"short"}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeEnrollmentAuthorityResolveRequest(
		"fingerprint", "CN=issuer", "01ab")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(
		invocation(db2contract.StageEnrollmentAuthorityResolve), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	found, authorityID, decodeErr :=
		db2contract.DecodeEnrollmentAuthorityResolveReply(body)
	if decodeErr != nil || found != 0 || authorityID != "" {
		t.Fatalf("found = %d, authority = %q", found, authorityID)
	}
}

func TestConsoleOIDCReplacesEveryFieldTogether(t *testing.T) {
	// A partial update that kept a previous admin claim while replacing the
	// issuer would grant administrative access on the strength of a claim from
	// a different provider.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeConsoleOidcPutRequest(
		"https://issuer", "aimee-console", "https://issuer/jwks", "groups", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageConsoleOidcPut), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeConsoleOidcPutReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	for _, column := range []string{
		"issuer", "audience", "jwks_url", "admin_claim", "admin_values", "updated_at",
	} {
		if !strings.Contains(store.lastSQL, column+" = EXCLUDED."+column) {
			t.Errorf("%s survives a replacement: %q", column, store.lastSQL)
		}
	}
	// A singleton: one console, one identity provider behind it. Without the
	// fixed key a second configuration would land beside the first and nothing
	// would choose between them.
	if !strings.Contains(store.lastSQL, "VALUES (1,") ||
		!strings.Contains(store.lastSQL, "ON CONFLICT (id)") {
		t.Errorf("the settings are no longer a singleton: %q", store.lastSQL)
	}
	if store.lastArgs[4] != "" {
		t.Errorf("an empty field was rewritten: %v", store.lastArgs[4])
	}
}
