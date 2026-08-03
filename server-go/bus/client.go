package bus

import (
	"encoding/binary"
	"errors"

	"golang.org/x/sys/unix"
)

// Attach wire, mirroring bus_attach_request_t (16 bytes) and
// bus_attach_reply_t (48 bytes).
const (
	attachReqMagic   uint32 = 0x51524241 // "ABRQ"
	attachReplyMagic uint32 = 0x50524241 // "ABRP"
	attachReqBytes          = 16
	attachReplyBytes        = 48
)

// AttachStatus mirrors bus_attach_status_t.
type AttachStatus uint32

const (
	AttachOK AttachStatus = iota
	AttachDeniedPolicy
	AttachDeniedVersion
	AttachDeniedNoSlot
	AttachProtocol
)

var (
	// ErrDenied is returned when the host refuses the attach.
	ErrDenied = errors.New("bus: attach denied")
	// ErrProtocol is returned on a malformed handshake.
	ErrProtocol = errors.New("bus: attach protocol error")
	// ErrEpoch is returned once the host has restarted.
	ErrEpoch = errors.New("bus: host restarted (epoch changed)")
	// ErrWouldBlock is returned when the outbound ring is full.
	ErrWouldBlock = errors.New("bus: would block")
	// ErrPayload is returned when a payload does not fit the inline budget.
	ErrPayload = errors.New("bus: payload too large for inline budget")
)

// Client is an attached bus client. It holds the mapped regions and, after
// attach, never touches the socket again.
type Client struct {
	Handle        uint32
	Status        AttachStatus
	slotSize      uint32
	inlineBudget  uint32
	queueCapacity uint32
	arenaSize     uint64
	attachedEpoch uint64

	control *Control
	qp      *QueuePair
	arena   []byte
	ctlMem  []byte
	qpMem   []byte

	pendingRead bool
}

// Event is a received event; Payload points into the shared inbound slot and is
// valid until the next Poll.
type Event struct {
	Frame   Frame
	Payload []byte
}

// Attach performs the handshake over a connected SOCK_SEQPACKET socket, receives
// the three descriptors, and maps the regions. The socket is not closed here.
func Attach(sock int) (*Client, error) {
	return AttachAs(sock, 0, 0)
}

// AttachAs performs an authenticated module attach. The host treats class and
// ref as claims only: its admission policy also binds them to SO_PEERCRED and
// the canonical executable path before granting event capabilities.
func AttachAs(sock int, principalClass uint32, principalRef uint32) (*Client, error) {
	var req [attachReqBytes]byte
	binary.LittleEndian.PutUint32(req[0:], attachReqMagic)
	binary.LittleEndian.PutUint16(req[4:], WireVersion) // min
	binary.LittleEndian.PutUint16(req[6:], WireVersion) // max
	binary.LittleEndian.PutUint32(req[8:], principalClass)
	binary.LittleEndian.PutUint32(req[12:], principalRef)
	if err := unix.Sendmsg(sock, req[:], nil, nil, 0); err != nil {
		return nil, err
	}

	replyBuf := make([]byte, attachReplyBytes)
	oob := make([]byte, unix.CmsgSpace(3*4))
	n, oobn, _, _, err := unix.Recvmsg(sock, replyBuf, oob, unix.MSG_CMSG_CLOEXEC)
	if err != nil {
		return nil, err
	}
	fds := parseRights(oob[:oobn])
	closeAll := func() {
		for _, fd := range fds {
			unix.Close(fd)
		}
	}
	if n != attachReplyBytes || binary.LittleEndian.Uint32(replyBuf[0:]) != attachReplyMagic {
		closeAll()
		return nil, ErrProtocol
	}
	status := AttachStatus(binary.LittleEndian.Uint32(replyBuf[4:]))
	if status != AttachOK {
		closeAll()
		return &Client{Status: status}, ErrDenied
	}
	if len(fds) != 3 {
		closeAll()
		return nil, ErrProtocol
	}

	c := &Client{
		Status:        status,
		Handle:        binary.LittleEndian.Uint32(replyBuf[8:]),
		slotSize:      binary.LittleEndian.Uint32(replyBuf[16:]),
		inlineBudget:  binary.LittleEndian.Uint32(replyBuf[20:]),
		queueCapacity: binary.LittleEndian.Uint32(replyBuf[24:]),
		arenaSize:     binary.LittleEndian.Uint64(replyBuf[32:]),
		attachedEpoch: binary.LittleEndian.Uint64(replyBuf[40:]),
	}

	// fds[0] control (read-only), fds[1] arena (rw), fds[2] queue pair (rw).
	ctlMem, err := unix.Mmap(fds[0], 0, ctlBytes, unix.PROT_READ, unix.MAP_SHARED)
	if err != nil {
		closeAll()
		return nil, err
	}
	arenaBytes := int(16 + c.arenaSize) // bus_arena_hdr_t (16) + usable bytes
	arena, err := unix.Mmap(fds[1], 0, arenaBytes, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		unix.Munmap(ctlMem)
		closeAll()
		return nil, err
	}
	qpBytesLen := qpairBytes(c.slotSize, c.queueCapacity)
	qpMem, err := unix.Mmap(fds[2], 0, qpBytesLen, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		unix.Munmap(ctlMem)
		unix.Munmap(arena)
		closeAll()
		return nil, err
	}
	closeAll() // the mappings hold their own references

	c.ctlMem, c.arena, c.qpMem = ctlMem, arena, qpMem
	if c.control, err = AttachControl(ctlMem); err != nil {
		c.Detach()
		return nil, err
	}
	if c.qp, err = AttachQueuePair(qpMem); err != nil {
		c.Detach()
		return nil, err
	}
	return c, nil
}

// Detach unmaps the regions.
func (c *Client) Detach() {
	if c.ctlMem != nil {
		unix.Munmap(c.ctlMem)
		c.ctlMem = nil
	}
	if c.arena != nil {
		unix.Munmap(c.arena)
		c.arena = nil
	}
	if c.qpMem != nil {
		unix.Munmap(c.qpMem)
		c.qpMem = nil
	}
}

// EpochChanged reports whether the host has restarted since attach.
func (c *Client) EpochChanged() bool {
	return c.control != nil && c.control.Epoch() != c.attachedEpoch
}

// Heartbeat writes a liveness value the host reads.
func (c *Client) Heartbeat(now uint64) { c.qp.SetHeartbeat(now) }

// ControlLost reports the sticky control_lost flag.
func (c *Client) ControlLost() bool { return c.qp.ControlLost() }

func (c *Client) emit(flags uint16, kind uint32, corr uint64, payload []byte) error {
	if c.control == nil {
		return ErrProtocol
	}
	if len(payload) > 0 &&
		(uint32(len(payload)) > c.inlineBudget || HdrLen+len(payload) > int(c.slotSize)) {
		return ErrPayload
	}
	if c.EpochChanged() {
		return ErrEpoch
	}
	slot := c.qp.Outbound.ProduceBegin()
	if slot == nil {
		return ErrWouldBlock
	}
	f := Frame{
		HdrFlags: flags, WireVersion: WireVersion, EventKind: kind,
		CorrelationID: corr, SrcHandle: c.Handle,
	}
	if len(payload) > 0 {
		f.HdrFlags |= FInline
		f.PayloadLen = uint32(len(payload))
		f.PayloadRef = HdrLen
	}
	if f.Encode(slot) != HdrLen {
		return ErrProtocol
	}
	if len(payload) > 0 {
		copy(slot[HdrLen:], payload)
	}
	c.qp.Outbound.ProduceCommit()
	return nil
}

// Publish sends a one-way notification.
func (c *Client) Publish(kind uint32, payload []byte) error {
	return c.emit(FNotification, kind, 0, payload)
}

// Request sends a correlated request; the reply arrives later via Poll.
func (c *Client) Request(kind uint32, correlation uint64, payload []byte) error {
	return c.RequestFragment(kind, correlation, payload, false)
}

// RequestFragment sends one ordered request fragment. More keeps the
// correlation in the request-assembly state until a final fragment arrives.
func (c *Client) RequestFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	if correlation == 0 {
		return ErrProtocol
	}
	flags := uint16(FRequest)
	if more {
		if len(payload) == 0 {
			return ErrProtocol
		}
		flags |= FMore
	}
	return c.emit(flags, kind, correlation, payload)
}

// Reply answers a request (a serving module).
func (c *Client) Reply(kind uint32, correlation uint64, payload []byte) error {
	return c.ReplyFragment(kind, correlation, payload, false)
}

// ReplyFragment sends one ordered reply fragment. More keeps the correlation
// pending for another reply fragment.
func (c *Client) ReplyFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	if correlation == 0 {
		return ErrProtocol
	}
	flags := uint16(FReply)
	if more {
		if len(payload) == 0 {
			return ErrProtocol
		}
		flags |= FMore
	}
	return c.emit(flags, kind, correlation, payload)
}

// Cancel cancels an outstanding request.
func (c *Client) Cancel(kind uint32, correlation uint64) error {
	if correlation == 0 {
		return ErrProtocol
	}
	return c.emit(FCancel, kind, correlation, nil)
}

// Poll takes the next inbound event, or returns ok=false if none. The returned
// Payload is valid until the next Poll.
func (c *Client) Poll() (Event, bool, error) {
	if c.control == nil {
		return Event{}, false, ErrProtocol
	}
	if c.EpochChanged() {
		return Event{}, false, ErrEpoch
	}
	if c.pendingRead {
		c.qp.Inbound.ConsumeCommit()
		c.pendingRead = false
	}
	slot := c.qp.Inbound.ConsumeBegin()
	if slot == nil {
		return Event{}, false, nil
	}
	var ev Event
	if Decode(slot, &ev.Frame) != OK {
		c.qp.Inbound.ConsumeCommit()
		return Event{}, false, nil
	}
	if ev.Frame.HdrFlags&FInline != 0 && ev.Frame.PayloadLen > 0 &&
		ev.Frame.PayloadRef+uint64(ev.Frame.PayloadLen) <= uint64(c.slotSize) {
		ev.Payload = slot[ev.Frame.PayloadRef : ev.Frame.PayloadRef+uint64(ev.Frame.PayloadLen)]
	}
	c.pendingRead = true
	return ev, true, nil
}

// parseRights extracts SCM_RIGHTS descriptors from a control-message buffer.
func parseRights(oob []byte) []int {
	if len(oob) == 0 {
		return nil
	}
	msgs, err := unix.ParseSocketControlMessage(oob)
	if err != nil {
		return nil
	}
	var fds []int
	for _, m := range msgs {
		if m.Header.Level == unix.SOL_SOCKET && m.Header.Type == unix.SCM_RIGHTS {
			f, err := unix.ParseUnixRights(&m)
			if err == nil {
				fds = append(fds, f...)
			}
		}
	}
	return fds
}

// qpairBytes mirrors bus_qpair_bytes: header, then two rings, each 64-aligned.
func qpairBytes(slotSize, capacity uint32) int {
	align64 := func(n int) int { return (n + 63) &^ 63 }
	ring := ringHdrBytes + int(slotSize)*int(capacity)
	inOff := align64(qpBytes)
	outOff := align64(inOff + ring)
	return outOff + ring
}
