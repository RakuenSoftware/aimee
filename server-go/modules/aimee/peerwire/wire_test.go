package peerwire

import (
	"encoding/binary"
	"errors"
	"reflect"
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
	// The ref is supplied by the module, not held here. These are the values at
	// ref 31; at ref 30 the same four stages give 11796-11799, and the formula
	// is what makes that a renumber rather than a rewrite.
	const ref uint32 = 31
	for _, tc := range []struct {
		stage uint32
		want  uint32
	}{
		{StageDelivery, 12033},
		{StageInbox, 12034},
		{StageGrant, 12035},
		{StageChannel, 12036},
	} {
		if got := EventKind(ref, tc.stage); got != tc.want {
			t.Errorf("EventKind(%d, %d) = %d; want %d", ref, tc.stage, got, tc.want)
		}
		if got := 4096 + ref*256 + tc.stage; got != tc.want {
			t.Errorf("formula for stage %d = %d; want %d", tc.stage, got, tc.want)
		}
	}
	// Stage ids are stable across a ref change; only the kinds move.
	if StageDelivery != 1 || StageInbox != 2 || StageGrant != 3 || StageChannel != 4 {
		t.Error("stage ids moved; the merge renumbers them deliberately, not incidentally")
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
	if len(cells) != MessageWidth {
		t.Fatalf("row width = %d; want %d", len(cells), MessageWidth)
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
	if _, err := MessageRows(cells[:MessageWidth-1]); !errors.Is(err, ErrRowCount) {
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
	if got := StatusFor(errors.New("something else")); got != StatusUnclassified {
		t.Errorf("unknown error = %v; want unclassified", got)
	}
}

// The status integer crosses the wire, so it is a contract rather than an
// ordering convenience. This pins every value.
//
// Without it, inserting a status in the middle renumbers every one after it and
// nothing fails: both sides still compile, both still pass their own tests, and
// a peer built against either one misreads the rest. That is the same
// append-do-not-insert rule the message cells follow, applied to the enum that
// was still using iota.
func TestStatusIntegersArePinned(t *testing.T) {
	pinned := map[int]Status{
		0:  StatusOK,
		1:  StatusNoPeer,
		2:  StatusDenied,
		3:  StatusInboxFull,
		4:  StatusHopLimit,
		5:  StatusCycle,
		6:  StatusTimeout,
		7:  StatusSelf,
		8:  StatusTooLong,
		9:  StatusLabelTaken,
		10: StatusUnknownSender,
		11: StatusBadRequest,
		12: StatusShutdown,
		13: StatusNoChannel,
		14: StatusNotMember,
		15: StatusChannelFull,
		16: StatusUnavailable,
		17: StatusUnclassified,
		18: StatusAtCapacity,
	}
	// The count is what catches an ADDITION. A status declared, given a String()
	// arm and used on the wire but never added here passes every other guard --
	// which is exactly what StatusUnclassified and StatusAtCapacity did.
	if len(pinned) != StatusCount {
		t.Fatalf("pinned %d statuses, package declares %d. A status added without "+
			"being pinned travels on the wire unchecked.", len(pinned), StatusCount)
	}
	for want, status := range pinned {
		if int(status) != want {
			t.Errorf("%s = %d; want %d. A status inserted in the middle renumbers "+
				"every one after it and the wire carries the integer.",
				status, int(status), want)
		}
	}

	// Every value must also have a distinct name: two statuses sharing one name
	// is the same collapse in the other direction, where an operator cannot tell
	// which refusal they got.
	seen := map[string]Status{}
	for i := 0; i < StatusCount; i++ {
		name := Status(i).String()
		if prev, dup := seen[name]; dup {
			t.Errorf("status %d and %d both name themselves %q", int(prev), i, name)
		}
		seen[name] = Status(i)
	}
	// An unrecognised status must NOT name itself as a real one. It used to
	// return "bad_request", which made an unknown value indistinguishable from a
	// refusal a caller acts on, and is what let StatusBadRequest live without an
	// arm of its own.
	if got := Status(9999).String(); got != "unknown_status" {
		t.Errorf("unrecognised status names itself %q; it must not alias a real one", got)
	}
}

// EVERY sentinel error must map to a status of its own choosing, never to the
// default.
//
// StatusFor's default used to return StatusBadRequest, so four errors that were
// never mapped -- ErrBadRequest itself, ErrChannelNameBad, ErrRegistryFull and
// ErrGrantsFull -- landed there silently. Two of those are CAPACITY: a caller
// told its request was malformed goes on correcting arguments that were never
// wrong, while the table stays full.
//
// The list is written out because Go cannot enumerate package-level vars. That
// makes it a transcription, which is the thing that goes stale, so the count is
// asserted too: an error added without being mapped fails here rather than
// passing while being reported as something it is not.
func TestEverySentinelErrorHasItsOwnStatus(t *testing.T) {
	sentinels := map[string]error{
		"ErrNoPeer": peer.ErrNoPeer, "ErrUnknownSender": peer.ErrUnknownSender,
		"ErrDenied": peer.ErrDenied, "ErrInboxFull": peer.ErrInboxFull,
		"ErrHopLimit": peer.ErrHopLimit, "ErrCycle": peer.ErrCycle,
		"ErrTimeout": peer.ErrTimeout, "ErrSelf": peer.ErrSelf,
		"ErrTooLong": peer.ErrTooLong, "ErrLabelTaken": peer.ErrLabelTaken,
		"ErrBadRequest": peer.ErrBadRequest, "ErrShutdown": peer.ErrShutdown,
		"ErrRegistryFull": peer.ErrRegistryFull, "ErrGrantsFull": peer.ErrGrantsFull,
		"ErrDirectoryUnavailable": peer.ErrDirectoryUnavailable,
		"ErrOwnerMismatch":        peer.ErrOwnerMismatch, "ErrNoChannel": peer.ErrNoChannel,
		"ErrNotMember": peer.ErrNotMember, "ErrChannelFull": peer.ErrChannelFull,
		"ErrChannelsFull": peer.ErrChannelsFull, "ErrChannelNameBad": peer.ErrChannelNameBad,
	}
	if len(sentinels) != peer.SentinelErrorCount {
		t.Fatalf("this list has %d errors, the package declares %d. An error added "+
			"without being mapped falls to the default and is reported as something "+
			"it is not.", len(sentinels), peer.SentinelErrorCount)
	}
	for name, err := range sentinels {
		if got := StatusFor(err); got == StatusUnclassified {
			t.Errorf("%s falls to the default; give it a status", name)
		}
	}

	// Capacity is not malformed input: they call for different repairs.
	for _, err := range []error{peer.ErrRegistryFull, peer.ErrGrantsFull} {
		if got := StatusFor(err); got != StatusAtCapacity {
			t.Errorf("a full table reported %v; want at_capacity", got)
		}
	}
}

// A struct richer than the message it is turned into loses the difference
// SILENTLY, and the loss is on the ENCODING side where there is no wire to be
// strict about: the decoder cannot object to a field the encoder never wrote.
//
// So the encoder is pinned against the struct. Add a field to peer.Message and
// this fails until MessageCells carries it, rather than the field simply not
// travelling and arriving zero-valued at the far end, indistinguishable from a
// sender who left it empty.
//
// The round-trip test does not cover this: it compares a literal the author
// wrote, so a field added and not set in that literal is zero on both sides and
// passes.
func TestMessageEncoderCarriesEveryField(t *testing.T) {
	fields := reflect.TypeOf(peer.Message{}).NumField()
	if fields != MessageWidth {
		t.Fatalf("peer.Message has %d fields, MessageWidth is %d. A field the encoder "+
			"does not carry arrives zero-valued and looks like one the sender left "+
			"empty.", fields, MessageWidth)
	}
	if got := len(MessageCells(peer.Message{})); got != MessageWidth {
		t.Errorf("MessageCells emitted %d cells; want %d", got, MessageWidth)
	}
}

// SendOptions is the case where the struct is DELIBERATELY richer than the
// wire, so the exclusion is named rather than assumed.
//
// WaitExpiry is not carried: the bus path never sets it, and the wait-for edge
// uses DefaultWaitExpiry. That is a decision, and writing it down is what makes
// the next field a decision too -- adding one fails here until somebody says
// which side of the line it falls on.
func TestSendOptionsWireCoverageIsDeliberate(t *testing.T) {
	const (
		carried  = 3 // ConversationID, Hop, ExpectReply
		excluded = 1 // WaitExpiry: bus callers take DefaultWaitExpiry
	)
	fields := reflect.TypeOf(peer.SendOptions{}).NumField()
	if fields != carried+excluded {
		t.Fatalf("peer.SendOptions has %d fields; this test accounts for %d carried "+
			"and %d deliberately excluded. A new field is silently dropped by the "+
			"bus path until it is classified.", fields, carried, excluded)
	}
}
