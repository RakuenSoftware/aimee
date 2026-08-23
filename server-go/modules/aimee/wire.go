package aimee

import (
	"encoding/binary"
	"errors"
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
	// messageWidth is the number of cells one peer message occupies. New cells
	// APPEND, so a reader built against an older width never has its existing
	// field numbering shift underneath it.
	messageWidth = 11
	// deliveryWidth is one channel fan-out outcome: session, status, message id.
	// Fixed width for the same reason messageWidth is -- a list reply carries no
	// row count and the caller divides.
	deliveryWidth = 3
)

var (
	// ErrWire is a malformed frame: bad counts, truncated cells, or trailing
	// bytes the frame did not declare.
	ErrWire = errors.New("aimee: malformed frame")
	// ErrNulInField is a field containing NUL. Values cross into C readers that
	// treat them as NUL-terminated, where an embedded NUL truncates silently.
	ErrNulInField = errors.New("aimee: field contains NUL")
	// ErrRowCount is a list reply whose cell count is not a whole number of rows.
	ErrRowCount = errors.New("aimee: reply is not a whole number of rows")
)

// Stage identifiers. Event kinds are DERIVED from the principal ref rather than
// written by hand: the bus formula is 4096 + principal_ref*256 + stage, so a
// module's identity and the kinds it answers on are one fact. Deriving them
// here means the two cannot drift.
const (
	PrincipalRef uint32 = 31

	StageDelivery uint32 = 1
	StageInbox    uint32 = 2
	StageGrant    uint32 = 3
	StageChannel  uint32 = 4
)

// EventKind returns the bus event kind for one of this module's stages.
func EventKind(stage uint32) uint32 { return 4096 + PrincipalRef*256 + stage }

// The kinds this module advertises. A stage declared in process-contracts.json
// but never advertised here is never available: calls return CAPABILITY_ABSENT
// while the module is plainly running.
// Values are DERIVED, so the trailing numbers below are informational only and
// follow PrincipalRef automatically. They are provisional: peer messaging is
// agreed to fold into ref 30 as stages 20/21/22, which makes them
// 11796/11797/11798. See "Merging with the db1 absorption" in the proposal.
var (
	EventDelivery = EventKind(StageDelivery) // ref 31 -> 12033
	EventInbox    = EventKind(StageInbox)    // ref 31 -> 12034
	EventGrant    = EventKind(StageGrant)    // ref 31 -> 12035
	EventChannel  = EventKind(StageChannel)  // ref 31 -> 12036
)

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
	StatusOK Status = iota
	StatusNoPeer
	StatusDenied
	StatusInboxFull
	StatusHopLimit
	StatusCycle
	StatusTimeout
	StatusSelf
	StatusTooLong
	StatusLabelTaken
	StatusUnknownSender
	StatusBadRequest
	StatusShutdown
	StatusNoChannel
	StatusNotMember
	StatusChannelFull
)

// StatusFor maps a registry error onto its wire status.
func StatusFor(err error) Status {
	switch {
	case err == nil:
		return StatusOK
	case errors.Is(err, peer.ErrNoPeer):
		return StatusNoPeer
	case errors.Is(err, peer.ErrDenied):
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
	default:
		return StatusBadRequest
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
	default:
		return "bad_request"
	}
}

// ---- framing -------------------------------------------------------------

func appendCells(frame []byte, cells []string) ([]byte, error) {
	var scratch [4]byte
	for _, c := range cells {
		if strings.ContainsRune(c, 0) {
			return nil, ErrNulInField
		}
		binary.LittleEndian.PutUint32(scratch[:], uint32(len(c)))
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
		if n > uint32(len(body)) || off+int(n) > len(body) {
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
	frame := make([]byte, 8, 8+16*len(cells))
	binary.LittleEndian.PutUint32(frame[0:], op)
	binary.LittleEndian.PutUint32(frame[4:], uint32(len(cells)))
	return appendCells(frame, cells)
}

// DecodeRequest splits a request frame into its operation and cells.
func DecodeRequest(body []byte) (uint32, []string, error) { return decodeCells(body) }

// EncodeResponse builds a response frame carrying a domain status.
func EncodeResponse(status Status, cells []string) ([]byte, error) {
	frame := make([]byte, 8, 8+16*len(cells))
	binary.LittleEndian.PutUint32(frame[0:], uint32(status))
	binary.LittleEndian.PutUint32(frame[4:], uint32(len(cells)))
	return appendCells(frame, cells)
}

// DecodeResponse splits a response frame into its status and cells.
func DecodeResponse(body []byte) (Status, []string, error) {
	head, cells, err := decodeCells(body)
	return Status(head), cells, err
}

// ---- scalars as text -----------------------------------------------------

func itoa(v int) string { return strconv.Itoa(v) }

func btoa(v bool) string {
	if v {
		return "1"
	}
	return "0"
}

func atoi(s string) (int, error) {
	if s == "" {
		return 0, nil
	}
	return strconv.Atoi(s)
}

func atob(s string) bool { return s == "1" || s == "true" }

func timeToText(t time.Time) string {
	if t.IsZero() {
		return ""
	}
	return strconv.FormatInt(t.UnixNano(), 10)
}

func textToTime(s string) time.Time {
	if s == "" {
		return time.Time{}
	}
	ns, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return time.Time{}
	}
	return time.Unix(0, ns).UTC()
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
		itoa(m.Hop),
		btoa(m.IsReply),
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
	return []string{d.Session, itoa(int(StatusFor(d.Err))), id}
}

// DeliveryRows divides a fan-out reply into outcomes, refusing a remainder.
func DeliveryRows(cells []string) ([]peer.Delivery, error) {
	if len(cells)%deliveryWidth != 0 {
		return nil, ErrRowCount
	}
	out := make([]peer.Delivery, 0, len(cells)/deliveryWidth)
	for i := 0; i < len(cells); i += deliveryWidth {
		code, err := atoi(cells[i+1])
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
	remaining, err := atoi(cells[0])
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
	if len(cells)%messageWidth != 0 {
		return nil, ErrRowCount
	}
	out := make([]peer.Message, 0, len(cells)/messageWidth)
	for i := 0; i < len(cells); i += messageWidth {
		row := cells[i : i+messageWidth]
		hop, err := atoi(row[7])
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
			IsReply:        atob(row[8]),
			SentAt:         textToTime(row[9]),
			Text:           row[10],
		})
	}
	return out, nil
}
