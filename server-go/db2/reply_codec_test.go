package db2

import (
	"bytes"
	"testing"
)

// A Go implementation of an operation ends by encoding a reply. Until these
// existed the generator emitted 446 request codecs and no generic reply codec
// at all, so a Go handler could read a request and had no way to answer.
//
// The wire vectors in tests/baselines pin the request side against C. The reply
// side has no C counterpart to compare against yet -- the C module encodes its
// replies from the same schema, and the parity gate is what will compare them --
// so what these check is that the codec is its own inverse and that it refuses
// what the schema says it must.

func TestRowReplyRoundTrips(t *testing.T) {
	rows := []EnrollmentListRow{
		{
			EnrollmentID:    7,
			EnrollmentScope: "replay-scope",
			CertFingerprint: "abc",
			CertSerialNorm:  "01",
			EnrollmentState: "active",
			IssuedAt:        "2026-01-01T00:00:00Z",
			LastSeenAt:      "2026-01-02T00:00:00Z",
			ExpiresAt:       "2027-01-01T00:00:00Z",
			RevokedAt:       "",
			AuthorityID:     "0123456789abcdef0123456789abcdef",
			LegacyRow:       1,
		},
		{EnrollmentID: 8, EnrollmentScope: "second", CertFingerprint: "def"},
	}
	encoded, err := EncodeEnrollmentListReply(rows)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	decoded, err := DecodeEnrollmentListReply(encoded)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(decoded) != len(rows) {
		t.Fatalf("decoded %d rows, want %d", len(decoded), len(rows))
	}
	for index := range rows {
		if decoded[index] != rows[index] {
			t.Errorf("row %d: %+v, want %+v", index, decoded[index], rows[index])
		}
	}
}

func TestEmptyRowReplyIsNotAnError(t *testing.T) {
	// A read that found nothing is an answer, and the commonest one. It has to
	// encode and decode as cleanly as a full page.
	encoded, err := EncodeEnrollmentListReply(nil)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	decoded, err := DecodeEnrollmentListReply(encoded)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(decoded) != 0 {
		t.Fatalf("decoded %d rows, want none", len(decoded))
	}
}

func TestRowReplyRefusesMoreRowsThanTheSchemaAllows(t *testing.T) {
	rows := make([]EnrollmentListRow, EnrollmentListMaxRows+1)
	if _, err := EncodeEnrollmentListReply(rows); err == nil {
		t.Fatal("encoded more rows than the ceiling permits")
	}
}

func TestRowReplyRefusesAFieldPastItsBound(t *testing.T) {
	// The bound is the row field's own, and it is checked per row rather than
	// on the first one.
	long := make([]byte, EnrollmentListEnrollmentScopeMax+1)
	for index := range long {
		long[index] = 'x'
	}
	rows := []EnrollmentListRow{
		{EnrollmentID: 1},
		{EnrollmentID: 2, EnrollmentScope: string(long)},
	}
	if _, err := EncodeEnrollmentListReply(rows); err == nil {
		t.Fatal("encoded a scope past its bound")
	}
}

func TestRowReplyRefusesACountPastTheCeiling(t *testing.T) {
	// The count drives the decode loop, so a malformed one is refused before
	// any row is read rather than after the buffer runs out.
	encoded, err := EncodeEnrollmentListReply([]EnrollmentListRow{{EnrollmentID: 1}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	broken := bytes.Clone(encoded)
	payload := broken[EnvelopeHeaderLen:]
	payload[0] = 0xff
	payload[1] = 0xff
	payload[2] = 0xff
	payload[3] = 0x7f
	if _, err := DecodeEnrollmentListReply(broken); err == nil {
		t.Fatal("decoded a count past the ceiling")
	}
}

func TestRowReplyRefusesTrailingBytes(t *testing.T) {
	encoded, err := EncodeEnrollmentListReply([]EnrollmentListRow{{EnrollmentID: 1}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, err := DecodeEnrollmentListReply(append(bytes.Clone(encoded), 0)); err == nil {
		t.Fatal("decoded a frame with a byte left over")
	}
}

func TestFlatReplyRoundTrips(t *testing.T) {
	encoded, err := EncodeDecisionLogInsertReply(
		1, 42, 4242, "options", "chosen", "rationale", "assumptions", "",
		"2026-08-22T09:00:00Z", "active", "", 0, "", "", 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	acknowledged, id, taskID, options, chosen, rationale, assumptions, outcome,
		createdAt, status, revisitWhen, supersedes, subject, author, policyID,
		decodeErr := DecodeDecisionLogInsertReply(encoded)
	if decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	if acknowledged != 1 || id != 42 || taskID != 4242 || options != "options" ||
		chosen != "chosen" || rationale != "rationale" || assumptions != "assumptions" ||
		outcome != "" || createdAt != "2026-08-22T09:00:00Z" || status != "active" ||
		revisitWhen != "" || supersedes != 0 || subject != "" || author != "" || policyID != 0 {
		t.Error("a field did not survive the round trip")
	}
}

func TestFlatReplyRefusesAFieldPastItsBound(t *testing.T) {
	long := make([]byte, DecisionLogInsertLoggedDecisionChosenMax+1)
	for index := range long {
		long[index] = 'x'
	}
	if _, err := EncodeDecisionLogInsertReply(
		1, 42, 4242, "options", string(long), "", "", "", "", "", "", 0, "", "", 0,
	); err == nil {
		t.Fatal("encoded a chosen value past its bound")
	}
}

func TestReplyRefusesAnotherOperationsFrame(t *testing.T) {
	// Every decoder checks the operation in the header, so a reply routed to the
	// wrong decoder is refused rather than reinterpreted -- the ids are unique
	// only within a family, which makes this the check that catches it.
	encoded, err := EncodeEnrollmentListReply([]EnrollmentListRow{{EnrollmentID: 1}})
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, decodeErr := DecodeProspectiveListArmedReply(encoded); decodeErr == nil {
		t.Fatal("one operation's reply decoded as another's")
	}
}
