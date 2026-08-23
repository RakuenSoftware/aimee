package peerwire

import (
	"encoding/binary"
	"errors"
	"math"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
)

// The aimee module speaks db1-fields-v2, the same counted-fields wire db1 uses,
// rather than a frame of its own:
//
//	Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//	Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count
//
// Lengths are little-endian and every value travels as text, so there is one
// representation per scalar and neither side negotiates widths or endianness.
// A list reply carries no row count: the module emits rowWidth*N cells and the
// caller divides, refusing a remainder rather than accepting a short final row.
//
// Reusing this wire is deliberate. An earlier draft of this file invented a
// bespoke length-prefixed frame because every OTHER module wire in the tree is
// fixed-size (routing 12 bytes, workspace 140) and none of them could carry an
// 8KB message body. That was the wrong conclusion: the fixed sizes are those
// modules' choices, not a limit of the transport — ModuleCaller fragments and
// reassembles in both directions, bounded only by bus.ModuleMessageMaxBody
// (16MiB). A second dialect would have bought nothing and cost every future
// reader an extra thing to learn.
const (
	// FieldsMax bounds a decoded frame so a corrupt count cannot make either
	// side allocate without limit before the count is checked.
	FieldsMax = 1 << 16
	// MessageWidth is the number of cells one peer message occupies. New cells
	// APPEND, so a reader built against an older width never has its existing
	// field numbering shift underneath it.
	MessageWidth = 11
	// DeliveryWidth is one channel fan-out outcome: session, status, message id.
	// Fixed width for the same reason MessageWidth is -- a list reply carries no
	// row count and the caller divides.
	DeliveryWidth = 3
)

var (
	// ErrWire is a malformed frame: bad counts, truncated cells, or trailing
	// bytes the frame did not declare.
	ErrWire = errors.New("aimee: malformed frame")
	// ErrNulInField is a field containing NUL. Values cross into C readers that
	// treat them as NUL-terminated, where an embedded NUL truncates silently.
	ErrNulInField = errors.New("aimee: field contains NUL")
	// ErrUnrepresentable is a value the frame's own fields cannot express: a
	// cell longer, or a cell count larger, than the u32 that carries it.
	//
	// The encoder REFUSES rather than converting, because the conversion does
	// not fail -- it wraps. A cell of 2^32+8 bytes writes the length 8, then
	// appends all 2^32+8 bytes, and the decoder reads eight of them as the cell
	// and the NEXT FOUR AS A LENGTH PREFIX. That is not truncation, it is frame
	// desynchronisation, and it turns cell CONTENT into framing: everything
	// after the wrap is structure the sender chose.
	//
	// Nothing can reach this today -- every cell that crosses is bounded well
	// below it -- and it is checked anyway, because "unreachable" is a property
	// of the current callers and the encoder is the thing that outlives them.
	ErrUnrepresentable = errors.New("aimee: value too large for its frame field")
	// ErrRowCount is a list reply whose cell count is not a whole number of rows.
	ErrRowCount = errors.New("aimee: reply is not a whole number of rows")
)

// Stage identifiers. Event kinds are DERIVED from the principal ref rather than
// written by hand: the bus formula is 4096 + principal_ref*256 + stage, so a
// module's identity and the kinds it answers on are one fact. Deriving them
// here means the two cannot drift.
const (
	StageDelivery uint32 = 1
	StageInbox    uint32 = 2
	StageGrant    uint32 = 3
	StageChannel  uint32 = 4
)

// EventKind returns the bus event kind for a stage under a principal ref.
//
// The ref is a PARAMETER rather than a constant here. A module has exactly one
// principal, and after the db1 absorption that ref belongs to the store rather
// than to peer messaging -- so the wire must not carry its own copy to disagree
// with. Kinds stay derived, which is what keeps identity and advertisement from
// drifting apart.
func EventKind(ref, stage uint32) uint32 { return 4096 + ref*256 + stage }

// Kinds are derived from the ref at the call site rather than held as package
// vars, since the ref is not this package's to know.
// Operations, numbered within their stage.
const (
	// StageDelivery
	OpSend       uint32 = 1
	OpReply      uint32 = 2
	OpCancelWait uint32 = 3

	// StageInbox
	OpInboxLen  uint32 = 1
	OpInboxPeek uint32 = 2
	OpInboxTake uint32 = 3

	// StageGrant
	OpGrant       uint32 = 1
	OpRevoke      uint32 = 2
	OpGrantExists uint32 = 3

	// StageChannel
	OpChannelJoin    uint32 = 1
	OpChannelLeave   uint32 = 2
	OpChannelSend    uint32 = 3
	OpChannelMembers uint32 = 4
)

// Status is the DOMAIN outcome of a well-formed call, carried in the response
// frame's status word.
//
// There are THREE outcome levels here, not two, and keeping them apart is what
// makes the governance tap readable:
//
//	ModuleStatus non-OK       I could not understand the question
//	                          (undecodable frame, unknown stage, cancellation)
//	fields-v2 status non-OK   I understood, and I REFUSE
//	                          (hop_limit, cycle, denied, inbox_full, no_peer)
//	StatusOK + outcome value  I understood, and the ANSWER is no
//	                          (grant_exists -> "0", an empty inbox -> no rows)
//
// The middle and bottom rows are the easy ones to conflate, and conflating them
// costs exactly what the split was built to prevent: a tap that sees a steady
// rate of non-OK cannot tell "policy is working as designed" from "something is
// wrong". So a question whose truthful answer is "no" — does this grant exist,
// is there mail — answers StatusOK and puts the no in a field. A request that
// did not happen and needs the caller to do something differently is a refusal.
//
// The line between them is whether the caller must change what it does.
// `denied` on a SEND is a refusal: the message did not go and the caller needs
// a grant. `grant_exists` returning false is an answer: nothing failed, and it
// is also the audit event that records someone asking.
//
// One case sits on the line and is deliberately a refusal: reading the inbox of
// a session that does not exist. "No such session" and "no mail" are
// indistinguishable as a count, and answering StatusOK with zero rows is how a
// caller polling for a reply waits forever on a session that was torn down.
//
// This matches db1, which draws the same three-way line: its family handler
// returns a domain status alongside bus.ModuleStatusOK, and carries outcomes
// like a jti replay as a VALUE at StatusOK rather than as a refusal.
type Status uint32

const (
	// Values are EXPLICIT, not iota. This integer is what crosses the wire, so
	// inserting a status in the middle would silently renumber every one after it
	// and a peer built against either side would misread the rest. New statuses
	// APPEND, exactly as message cells do; the numbers are a contract, not an
	// ordering convenience.
	StatusOK            Status = 0
	StatusNoPeer        Status = 1
	StatusDenied        Status = 2
	StatusInboxFull     Status = 3
	StatusHopLimit      Status = 4
	StatusCycle         Status = 5
	StatusTimeout       Status = 6
	StatusSelf          Status = 7
	StatusTooLong       Status = 8
	StatusLabelTaken    Status = 9
	StatusUnknownSender Status = 10
	StatusBadRequest    Status = 11
	StatusShutdown      Status = 12
	StatusNoChannel     Status = 13
	StatusNotMember     Status = 14
	StatusChannelFull   Status = 15
	// StatusUnavailable is a dependency that did not ANSWER, which is not the
	// same as answering no. It is the one status here a caller should RETRY on:
	// every other refusal is a fact about the request, this one is a fact about
	// the moment.
	StatusUnavailable Status = 16
	// StatusUnclassified is an error the module could not name. It is NOT
	// bad_request: telling a caller its request was malformed when the module
	// merely failed in a way it does not classify sends them to fix a call that
	// was fine. One says "change what you sent", the other says "this is a bug
	// worth reporting".
	StatusUnclassified Status = 17
	// StatusAtCapacity is a TABLE at its ceiling, as distinct from a malformed
	// request. Reported as bad_request, a caller goes on correcting arguments
	// that were never wrong; reported as capacity, it waits or stops creating.
	StatusAtCapacity Status = 18
	// StatusNoDirectory is this module having NO session directory wired, so it
	// cannot answer any question about who exists. Distinct from
	// StatusUnavailable, which is the one status a caller should retry on: this
	// one is permanent until the module is rebuilt differently, and a caller
	// retrying against it never stops.
	//
	// It exists because the deployed module was in exactly this state and said
	// unknown_sender instead -- an answer about the CALLER'S session, from a
	// module that had no way to know about any session at all. Every refusal
	// check in the container run passed against it.
	StatusNoDirectory Status = 19
	// StatusDirectoryRefused is the directory understanding a request and
	// REFUSING it: the module asked for something the directory will not accept.
	//
	// Distinct from unavailable, which means retry, and from bad_request, which
	// blames the caller. This one is neither -- the caller's request was fine and
	// the defective one was the module's, so the caller can only report it. It is
	// also not unclassified: that status means an error the module could not
	// NAME, and this one has a name.
	//
	// It exists because a refusal was reaching callers as `unavailable`. The
	// registry wrapped anything that was not absence into "did not answer", so a
	// permanent refusal became the one status that means try again -- a retry
	// loop over a request that can never succeed.
	StatusDirectoryRefused Status = 20

	// StatusCount is one past the highest status, and the pinning test asserts
	// against it.
	//
	// Without it the pinned list is a transcription that does not know a status
	// was added, which is not hypothetical: StatusUnclassified and
	// StatusAtCapacity were both declared, given String() arms, used on the wire,
	// and never pinned -- and every guard stayed green. The compiler catches an
	// insert that duplicates a case; the pinned values catch a renumber into a
	// gap; only this catches an addition that never joined the list. All three are
	// needed, and each is invisible to the others.
	StatusCount = 21
)

// StatusFor maps a registry error onto its wire status.
func StatusFor(err error) Status {
	switch {
	case err == nil:
		return StatusOK
	case errors.Is(err, peer.ErrNoPeer):
		return StatusNoPeer
	case errors.Is(err, peer.ErrDenied), errors.Is(err, peer.ErrOwnerMismatch):
		// Both are "you are not permitted to act here", which is the fact a
		// caller acts on; the errors stay distinct so a log says which.
		return StatusDenied
	case errors.Is(err, peer.ErrInboxFull):
		return StatusInboxFull
	case errors.Is(err, peer.ErrHopLimit):
		return StatusHopLimit
	case errors.Is(err, peer.ErrCycle):
		return StatusCycle
	case errors.Is(err, peer.ErrTimeout):
		return StatusTimeout
	case errors.Is(err, peer.ErrSelf):
		return StatusSelf
	case errors.Is(err, peer.ErrTooLong):
		return StatusTooLong
	case errors.Is(err, peer.ErrLabelTaken):
		return StatusLabelTaken
	case errors.Is(err, peer.ErrUnknownSender):
		return StatusUnknownSender
	case errors.Is(err, peer.ErrShutdown):
		return StatusShutdown
	case errors.Is(err, peer.ErrNoChannel):
		return StatusNoChannel
	case errors.Is(err, peer.ErrNotMember):
		return StatusNotMember
	case errors.Is(err, peer.ErrChannelFull), errors.Is(err, peer.ErrChannelsFull):
		return StatusChannelFull
	// Ordered before ErrDirectoryUnavailable deliberately: "there is none" must
	// not be answered by the arm that means "it did not reply this time".
	case errors.Is(err, peer.ErrNoDirectory):
		return StatusNoDirectory
	// The directory understood and REFUSED, which is a defect in the request
	// this module built rather than anything the caller sent. Unclassified
	// rather than bad_request for exactly that reason -- bad_request tells the
	// caller to fix a call that was fine -- and never unavailable, which would
	// have them retry something that can never be accepted.
	case errors.Is(err, peer.ErrDirectoryRefused):
		return StatusDirectoryRefused
	case errors.Is(err, peer.ErrDirectoryUnavailable):
		return StatusUnavailable
	case errors.Is(err, peer.ErrRegistryFull), errors.Is(err, peer.ErrGrantsFull):
		return StatusAtCapacity
	case errors.Is(err, peer.ErrBadRequest), errors.Is(err, peer.ErrChannelNameBad):
		return StatusBadRequest
	default:
		return StatusUnclassified
	}
}

// String names a status for logs and the audit stream.
func (s Status) String() string {
	switch s {
	case StatusOK:
		return "ok"
	case StatusNoPeer:
		return "no_peer"
	case StatusDenied:
		return "denied"
	case StatusInboxFull:
		return "inbox_full"
	case StatusHopLimit:
		return "hop_limit"
	case StatusCycle:
		return "cycle"
	case StatusTimeout:
		return "timeout"
	case StatusSelf:
		return "self"
	case StatusTooLong:
		return "too_long"
	case StatusLabelTaken:
		return "label_taken"
	case StatusUnknownSender:
		return "unknown_sender"
	case StatusShutdown:
		return "shutdown"
	case StatusNoChannel:
		return "no_channel"
	case StatusNotMember:
		return "not_member"
	case StatusChannelFull:
		return "channel_full"
	case StatusUnavailable:
		return "unavailable"
	case StatusBadRequest:
		return "bad_request"
	case StatusUnclassified:
		return "unclassified"
	case StatusAtCapacity:
		return "at_capacity"
	case StatusNoDirectory:
		return "no_directory"
	case StatusDirectoryRefused:
		return "directory_refused"
	default:
		// Deliberately NOT the name of a real status. Falling through to
		// "bad_request" made an unrecognised value indistinguishable from one a
		// caller acts on, and it is what let StatusBadRequest live without an
		// arm of its own.
		//
		// This default is also what makes the compile-time guard on these
		// constants work: every status having an arm is what turns an inserted
		// value into a duplicate case the compiler refuses. A status added
		// WITHOUT an arm compiles, lands here, and only the pinning test catches
		// it -- so that arm is load-bearing, not decoration.
		return "unknown_status"
	}
}

// ---- framing -------------------------------------------------------------

// u32 narrows a length or count to the field that carries it, refusing anything
// the field cannot hold.
//
// Split out as a function so the refusal is testable: proving it by encoding a
// four-gigabyte cell would need a four-gigabyte cell, which is why this kind of
// check normally goes untested and therefore unwritten.
func u32(n int) (uint32, error) {
	if n < 0 || uint64(n) > math.MaxUint32 {
		return 0, ErrUnrepresentable
	}
	return uint32(n), nil
}

func appendCells(frame []byte, cells []string) ([]byte, error) {
	var scratch [4]byte
	for _, c := range cells {
		if strings.ContainsRune(c, 0) {
			return nil, ErrNulInField
		}
		n, err := u32(len(c))
		if err != nil {
			return nil, err
		}
		binary.LittleEndian.PutUint32(scratch[:], n)
		frame = append(frame, scratch[:]...)
		frame = append(frame, c...)
	}
	return frame, nil
}

func decodeCells(body []byte) (uint32, []string, error) {
	if len(body) < 8 {
		return 0, nil, ErrWire
	}
	head := binary.LittleEndian.Uint32(body[0:])
	count := binary.LittleEndian.Uint32(body[4:])
	if count > FieldsMax {
		return 0, nil, ErrWire
	}
	cells := make([]string, 0, count)
	off := 8
	for i := uint32(0); i < count; i++ {
		if off+4 > len(body) {
			return 0, nil, ErrWire
		}
		n := binary.LittleEndian.Uint32(body[off:])
		off += 4
		// Compared in the WIDER type on both sides. The earlier form narrowed
		// len(body) to a u32, which was safe only by accident: a body over 4GiB
		// wraps to a small number, and the comparison then refuses almost
		// everything. Fail-closed, so it was never a bug -- but it was correct
		// for a reason no reader could see, and the second clause was doing the
		// real work regardless.
		if uint64(n) > uint64(len(body)-off) {
			return 0, nil, ErrWire
		}
		cells = append(cells, string(body[off:off+int(n)]))
		off += int(n)
	}
	// Trailing bytes are an error rather than ignored: accepting them would let
	// a sender believe it passed a field this version never read.
	if off != len(body) {
		return 0, nil, ErrWire
	}
	return head, cells, nil
}

// EncodeRequest builds a request frame for one operation.
func EncodeRequest(op uint32, cells []string) ([]byte, error) {
	count, err := u32(len(cells))
	if err != nil {
		return nil, err
	}
	frame := make([]byte, 8, 8+16*len(cells))
	binary.LittleEndian.PutUint32(frame[0:], op)
	binary.LittleEndian.PutUint32(frame[4:], count)
	return appendCells(frame, cells)
}

// DecodeRequest splits a request frame into its operation and cells.
func DecodeRequest(body []byte) (uint32, []string, error) { return decodeCells(body) }

// EncodeResponse builds a response frame carrying a domain status.
func EncodeResponse(status Status, cells []string) ([]byte, error) {
	frame := make([]byte, 8, 8+16*len(cells))
	count, err := u32(len(cells))
	if err != nil {
		return nil, err
	}
	binary.LittleEndian.PutUint32(frame[0:], uint32(status))
	binary.LittleEndian.PutUint32(frame[4:], count)
	return appendCells(frame, cells)
}

// DecodeResponse splits a response frame into its status and cells.
func DecodeResponse(body []byte) (Status, []string, error) {
	head, cells, err := decodeCells(body)
	return Status(head), cells, err
}

// DecodeReply decodes a fields-v2 reply WITHOUT typing its status word.
//
// For replies from another module, whose status enum is its own. db1's status 1
// is MISSING and 4 is FAILED; this package's 1 is no_peer and 4 is hop_limit.
// The integers collide and the meanings do not, so handing a db1 reply to
// DecodeResponse would produce a peerwire.Status that reads as a sensible value
// and means something else entirely -- a wrong answer with no wrong-looking
// step in it.
//
// Returning the raw u32 forces the caller to name which enum it is reading.
func DecodeReply(body []byte) (uint32, []string, error) { return decodeCells(body) }

// ---- scalars as text -----------------------------------------------------

// Itoa renders an integer as the decimal text the wire carries.
func Itoa(v int) string { return strconv.Itoa(v) }

// Btoa renders a bool as the wire's "1"/"0".
func Btoa(v bool) string {
	if v {
		return "1"
	}
	return "0"
}

// Atoi parses a decimal text cell, treating empty as zero.
func Atoi(s string) (int, error) {
	if s == "" {
		return 0, nil
	}
	return strconv.Atoi(s)
}

// Atob parses a wire bool, refusing anything that is not one.
//
// It used to answer plain `false` for an unrecognised cell, which made "false"
// and "that was not a bool" the same answer -- while its sibling Atoi returned
// an error that the capability turned into bad_request. The same frame had a
// validated integer cell and an unvalidated boolean one.
//
// The empty string is a legitimate false: the encoder writes "0", but a caller
// that omits an optional flag is saying no rather than saying nonsense.
func Atob(s string) (bool, error) {
	switch s {
	case "1", "true":
		return true, nil
	case "0", "false", "":
		return false, nil
	default:
		return false, ErrWire
	}
}

func timeToText(t time.Time) string {
	if t.IsZero() {
		return ""
	}
	return strconv.FormatInt(t.UnixNano(), 10)
}

// textToTime parses a wire timestamp, refusing a cell that is not one.
//
// The empty string IS a zero time -- that is what timeToText writes for one --
// so those two agree. What must not agree is a CORRUPT cell and an unset one:
// this used to map an unparseable timestamp to the zero time as well, so a
// damaged SentAt arrived as "this message has no send time" and every message
// row decoded successfully no matter what was in that cell.
func textToTime(s string) (time.Time, error) {
	if s == "" {
		return time.Time{}, nil
	}
	ns, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return time.Time{}, ErrWire
	}
	return time.Unix(0, ns).UTC(), nil
}

// ---- message rows --------------------------------------------------------

// MessageCells renders one peer message as its fixed-width row.
func MessageCells(m peer.Message) []string {
	return []string{
		m.ID,
		m.CorrelationID,
		m.ConversationID,
		m.FromSession,
		m.FromOwner,
		m.FromLabel,
		m.OriginSession,
		Itoa(m.Hop),
		Btoa(m.IsReply),
		timeToText(m.SentAt),
		m.Text,
	}
}

// DeliveryCells renders one channel fan-out outcome as its fixed-width row.
// The message id is empty on a refusal; the status names why.
func DeliveryCells(d peer.Delivery) []string {
	id := ""
	if d.Err == nil {
		id = d.Message.ID
	}
	return []string{d.Session, Itoa(int(StatusFor(d.Err))), id}
}

// DeliveryRows divides a fan-out reply into outcomes, refusing a remainder.
func DeliveryRows(cells []string) ([]peer.Delivery, error) {
	if len(cells)%DeliveryWidth != 0 {
		return nil, ErrRowCount
	}
	out := make([]peer.Delivery, 0, len(cells)/DeliveryWidth)
	for i := 0; i < len(cells); i += DeliveryWidth {
		code, err := Atoi(cells[i+1])
		if err != nil {
			return nil, ErrWire
		}
		d := peer.Delivery{Session: cells[i]}
		if Status(code) == StatusOK {
			d.Message = peer.Message{ID: cells[i+2]}
		} else {
			d.Err = errors.New("peer: " + Status(code).String())
		}
		out = append(out, d)
	}
	return out, nil
}

// TakeReply splits an OpInboxTake reply into the remaining count and the
// messages drained. The leading cell is the remainder, so a caller can tell a
// complete drain from a capped one without a second call.
func TakeReply(cells []string) (int, []peer.Message, error) {
	if len(cells) == 0 {
		return 0, nil, ErrWire
	}
	remaining, err := Atoi(cells[0])
	if err != nil {
		return 0, nil, ErrWire
	}
	msgs, err := MessageRows(cells[1:])
	if err != nil {
		return 0, nil, err
	}
	return remaining, msgs, nil
}

// MessageRows divides a list reply into messages, refusing a remainder rather
// than returning a short final row.
func MessageRows(cells []string) ([]peer.Message, error) {
	if len(cells)%MessageWidth != 0 {
		return nil, ErrRowCount
	}
	out := make([]peer.Message, 0, len(cells)/MessageWidth)
	for i := 0; i < len(cells); i += MessageWidth {
		row := cells[i : i+MessageWidth]
		hop, err := Atoi(row[7])
		if err != nil {
			return nil, ErrWire
		}
		isReply, err := Atob(row[8])
		if err != nil {
			return nil, ErrWire
		}
		sentAt, err := textToTime(row[9])
		if err != nil {
			return nil, ErrWire
		}
		out = append(out, peer.Message{
			ID:             row[0],
			CorrelationID:  row[1],
			ConversationID: row[2],
			FromSession:    row[3],
			FromOwner:      row[4],
			FromLabel:      row[5],
			OriginSession:  row[6],
			Hop:            hop,
			IsReply:        isReply,
			SentAt:         sentAt,
			Text:           row[10],
		})
	}
	return out, nil
}
