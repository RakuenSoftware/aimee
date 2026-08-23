package aimee

import (
	"encoding/binary"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
)

// Event kinds must satisfy the bus formula for this module's principal ref.
// Hand-written kinds are how a module ends up advertising one thing and
// declaring another; deriving them makes that impossible in this file, and this
// test pins the derivation itself.
func TestEventKindsFollowTheBusFormula(t *testing.T) {
	if PrincipalRef != 31 {
		t.Fatalf("PrincipalRef = %d; the inventory declares 31", PrincipalRef)
	}
	for _, tc := range []struct {
		stage uint32
		want  uint32
	}{
		{StageDelivery, 12033},
		{StageInbox, 12034},
		{StageGrant, 12035},
	} {
		if got := EventKind(tc.stage); got != tc.want {
			t.Errorf("EventKind(%d) = %d; want %d", tc.stage, got, tc.want)
		}
		if got := 4096 + PrincipalRef*256 + tc.stage; got != tc.want {
			t.Errorf("formula for stage %d = %d; want %d", tc.stage, got, tc.want)
		}
	}
}

func TestRequestRoundTrip(t *testing.T) {
	cells := []string{"A", "B", "hello", "", "0", "1"}
	frame, err := EncodeRequest(OpSend, cells)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	op, got, err := DecodeRequest(frame)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if op != OpSend {
		t.Errorf("op = %d", op)
	}
	if strings.Join(got, "\x1f") != strings.Join(cells, "\x1f") {
		t.Errorf("cells = %q; want %q", got, cells)
	}
}

func TestResponseRoundTripCarriesDomainStatus(t *testing.T) {
	frame, err := EncodeResponse(StatusCycle, nil)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	status, cells, err := DecodeResponse(frame)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if status != StatusCycle {
		t.Errorf("status = %v; want cycle", status)
	}
	if len(cells) != 0 {
		t.Errorf("cells = %q; want none", cells)
	}
}

// Empty strings must survive: "no label" and "" are different facts and the
// framing must not collapse them into a missing cell.
func TestEmptyCellsSurvive(t *testing.T) {
	cells := []string{"", "x", ""}
	frame, err := EncodeRequest(OpInboxLen, cells)
	if err != nil {
		t.Fatal(err)
	}
	_, got, err := DecodeRequest(frame)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 3 || got[0] != "" || got[1] != "x" || got[2] != "" {
		t.Fatalf("cells = %q", got)
	}
}

// A NUL in a field is refused rather than encoded: values cross into C readers
// that treat them as NUL-terminated, where an embedded NUL truncates silently.
func TestNulInFieldRefused(t *testing.T) {
	if _, err := EncodeRequest(OpSend, []string{"a\x00b"}); !errors.Is(err, ErrNulInField) {
		t.Fatalf("err = %v; want ErrNulInField", err)
	}
	if _, err := EncodeResponse(StatusOK, []string{"a\x00b"}); !errors.Is(err, ErrNulInField) {
		t.Fatalf("response err = %v; want ErrNulInField", err)
	}
}

// Malformed frames are rejected rather than partially accepted. Trailing bytes
// matter specifically: accepting them would let a sender believe it passed a
// field this version never read.
func TestMalformedFramesRejected(t *testing.T) {
	good, err := EncodeRequest(OpSend, []string{"A", "B"})
	if err != nil {
		t.Fatal(err)
	}
	for name, body := range map[string][]byte{
		"empty":          {},
		"header only":    good[:4],
		"truncated cell": good[:len(good)-1],
		"trailing bytes": append(append([]byte{}, good...), 0x00),
	} {
		if _, _, err := DecodeRequest(body); !errors.Is(err, ErrWire) {
			t.Errorf("%s: err = %v; want ErrWire", name, err)
		}
	}

	// A count that would over-allocate is refused on the count, before any
	// allocation proportional to it.
	huge := make([]byte, 8)
	binary.LittleEndian.PutUint32(huge[0:], OpSend)
	binary.LittleEndian.PutUint32(huge[4:], FieldsMax+1)
	if _, _, err := DecodeRequest(huge); !errors.Is(err, ErrWire) {
		t.Errorf("oversized count: err = %v; want ErrWire", err)
	}

	// A cell length larger than the frame is refused rather than slicing.
	lying := make([]byte, 12)
	binary.LittleEndian.PutUint32(lying[0:], OpSend)
	binary.LittleEndian.PutUint32(lying[4:], 1)
	binary.LittleEndian.PutUint32(lying[8:], 1<<20)
	if _, _, err := DecodeRequest(lying); !errors.Is(err, ErrWire) {
		t.Errorf("lying length: err = %v; want ErrWire", err)
	}
}

func TestMessageRowRoundTrip(t *testing.T) {
	sent := time.Unix(0, 1700000000123456789).UTC()
	m := peer.Message{
		ID: "pmsg-1", CorrelationID: "pmsg-1", ConversationID: "conv-1",
		FromSession: "A", FromOwner: "uid:1000", FromLabel: "author",
		OriginSession: "A", Hop: 3, IsReply: true, SentAt: sent,
		Text: "body with unicode: ✓ and a quote \"",
	}
	cells := MessageCells(m)
	if len(cells) != messageWidth {
		t.Fatalf("row width = %d; want %d", len(cells), messageWidth)
	}
	got, err := MessageRows(cells)
	if err != nil || len(got) != 1 {
		t.Fatalf("rows: %v (%d)", err, len(got))
	}
	if got[0] != m {
		t.Errorf("round trip lost fields:\n got %+v\nwant %+v", got[0], m)
	}
}

// A list reply carries no row count, so the caller divides — and must refuse a
// remainder rather than hand back a short final row.
func TestMessageRowsRefusesRemainder(t *testing.T) {
	cells := MessageCells(peer.Message{ID: "x"})
	if _, err := MessageRows(cells[:messageWidth-1]); !errors.Is(err, ErrRowCount) {
		t.Fatalf("short row: err = %v; want ErrRowCount", err)
	}
	two := append(MessageCells(peer.Message{ID: "a"}), MessageCells(peer.Message{ID: "b"})...)
	got, err := MessageRows(two)
	if err != nil || len(got) != 2 {
		t.Fatalf("two rows: %v (%d)", err, len(got))
	}
}

// Every registry error must map to a distinct, non-default status: a refusal
// that arrives as bad_request tells an operator nothing about why.
func TestStatusForCoversEveryRefusal(t *testing.T) {
	for err, want := range map[error]Status{
		nil:                   StatusOK,
		peer.ErrNoPeer:        StatusNoPeer,
		peer.ErrDenied:        StatusDenied,
		peer.ErrInboxFull:     StatusInboxFull,
		peer.ErrHopLimit:      StatusHopLimit,
		peer.ErrCycle:         StatusCycle,
		peer.ErrTimeout:       StatusTimeout,
		peer.ErrSelf:          StatusSelf,
		peer.ErrTooLong:       StatusTooLong,
		peer.ErrLabelTaken:    StatusLabelTaken,
		peer.ErrUnknownSender: StatusUnknownSender,
		peer.ErrShutdown:      StatusShutdown,
	} {
		if got := StatusFor(err); got != want {
			t.Errorf("StatusFor(%v) = %v; want %v", err, got, want)
		}
		if want.String() == "bad_request" && err != nil {
			t.Errorf("%v collapsed into bad_request", err)
		}
	}
	if got := StatusFor(errors.New("something else")); got != StatusBadRequest {
		t.Errorf("unknown error = %v; want bad_request", got)
	}
}
