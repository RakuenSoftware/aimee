package bus

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sync"

	"golang.org/x/sys/unix"
)

// Attach wire, mirroring bus_attach_request_t (16 bytes) and
// bus_attach_reply_t (48 bytes).
const (
	attachReqMagic   uint32 = 0x51524241 // "ABRQ"
	attachReplyMagic uint32 = 0x50524241 // "ABRP"
	attachReqBytes          = 16
	attachReplyBytes        = 48

	// Field offsets, frozen by src/tests/fixtures/bus/layout_vectors.tsv.
	attachReqOffMagic          = 0
	attachReqOffWireMin        = 4
	attachReqOffWireMax        = 6
	attachReqOffPrincipalClass = 8
	attachReqOffPrincipalRef   = 12
	attachReplyOffMagic        = 0
	attachReplyOffStatus       = 4
	attachReplyOffHandle       = 8
	attachReplyOffWireVersion  = 12
	attachReplyOffSlot         = 16
	attachReplyOffInline       = 20
	attachReplyOffQueueCap     = 24
	attachReplyOffReserved     = 28
	attachReplyOffArena        = 32
	attachReplyOffEpoch        = 40

	// A successful attach carries exactly these descriptors, in this order.
	attachDescriptors = 3
	attachFdControl   = 0
	attachFdArena     = 1
	attachFdQpair     = 2
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

// String names the refusal, so a log line says which of the four it was.
//
// The host distinguishes four and they call for different things: a policy
// refusal means no grant admits this principal or its executable does not
// match; a version refusal means the two ends disagree about the protocol;
// no-slot means the host is full and it is worth retrying. A bare "attach
// denied" for all of them sends a reader to the grant file even when the grant
// was never the problem, which src/tests/test_plugin_grant_provisioning.c names
// as the reason that test exists.
func (s AttachStatus) String() string {
	switch s {
	case AttachOK:
		return "ok"
	case AttachDeniedPolicy:
		return "denied by grant policy: no grant admits this principal, or the " +
			"grant's executable= does not match the running binary"
	case AttachDeniedVersion:
		return "denied on protocol version"
	case AttachDeniedNoSlot:
		return "denied: the host has no free slot"
	case AttachProtocol:
		return "protocol error"
	}
	return fmt.Sprintf("unknown attach status %d", uint32(s))
}

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

	// D12: arena reads across the process boundary. leaseView is nil when the
	// host predates it; arena events then arrive with no payload, as before.
	leaseView    []byte
	arenaUsable  []byte
	pendingLease *leaseRef
	releases     []leaseRef

	// emitMu serialises writers to the single-producer outbound ring. See emit.
	emitMu sync.Mutex
}

// leaseRef names an arena consumer ref the client owes back to the host.
type leaseRef struct{ id, generation uint32 }

// maxPendingReleases bounds refs owed but not yet sent because the outbound
// ring was full; a client this far behind stops taking arena events until its
// outbound drains.
const maxPendingReleases = 64

// Event is a received event. Payload points into the shared inbound slot, or
// for an arena-placed payload into the arena span its lease names (D12), and is
// valid until the next Poll; the next Poll also returns the arena ref.
type Event struct {
	Frame   Frame
	Payload []byte
}

// maxRegionBytes bounds an announced arena so the mapping length cannot
// overflow int; the C host caps the arena far below this (BUS_ARENA_MAX_SIZE).
const maxRegionBytes = 1 << 40

func fdSize(fd int) (int64, error) {
	var st unix.Stat_t
	if err := unix.Fstat(fd, &st); err != nil {
		return 0, err
	}
	return st.Size, nil
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
	binary.LittleEndian.PutUint32(req[attachReqOffMagic:], attachReqMagic)
	binary.LittleEndian.PutUint16(req[attachReqOffWireMin:], WireVersion)
	binary.LittleEndian.PutUint16(req[attachReqOffWireMax:], WireVersion)
	binary.LittleEndian.PutUint32(req[attachReqOffPrincipalClass:], principalClass)
	binary.LittleEndian.PutUint32(req[attachReqOffPrincipalRef:], principalRef)
	if err := unix.Sendmsg(sock, req[:], nil, nil, 0); err != nil {
		return nil, err
	}

	replyBuf := make([]byte, attachReplyBytes)
	oob := make([]byte, unix.CmsgSpace(attachDescriptors*4))
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
	if n != attachReplyBytes || binary.LittleEndian.Uint32(replyBuf[attachReplyOffMagic:]) != attachReplyMagic {
		closeAll()
		return nil, ErrProtocol
	}
	status := AttachStatus(binary.LittleEndian.Uint32(replyBuf[attachReplyOffStatus:]))
	if status != AttachOK {
		closeAll()
		return &Client{Status: status}, fmt.Errorf("%w: %s", ErrDenied, status)
	}
	if len(fds) != attachDescriptors {
		closeAll()
		return nil, ErrProtocol
	}

	c := &Client{
		Status:        status,
		Handle:        binary.LittleEndian.Uint32(replyBuf[attachReplyOffHandle:]),
		slotSize:      binary.LittleEndian.Uint32(replyBuf[attachReplyOffSlot:]),
		inlineBudget:  binary.LittleEndian.Uint32(replyBuf[attachReplyOffInline:]),
		queueCapacity: binary.LittleEndian.Uint32(replyBuf[attachReplyOffQueueCap:]),
		arenaSize:     binary.LittleEndian.Uint64(replyBuf[attachReplyOffArena:]),
		attachedEpoch: binary.LittleEndian.Uint64(replyBuf[attachReplyOffEpoch:]),
	}

	if binary.LittleEndian.Uint32(replyBuf[attachReplyOffWireVersion:]) != uint32(WireVersion) ||
		c.slotSize <= HdrLen || c.inlineBudget > c.slotSize ||
		c.arenaSize == 0 || c.arenaSize > maxRegionBytes {
		closeAll()
		return nil, ErrProtocol
	}

	// fds[0] control (read-only), fds[1] arena (rw), fds[2] queue pair (rw).
	// Each memfd must cover the geometry the reply announced: a mapping past
	// the end of the file would fault on first touch rather than fail here.
	// A D12 host's control region carries the lease view after the header; an
	// older host's is the header alone. Map what is there.
	ctlSize, err := fdSize(fds[attachFdControl])
	if err != nil {
		closeAll()
		return nil, err
	}
	ctlLen := ctlBytes
	if ctlSize >= ctlBytesWithView {
		ctlLen = ctlBytesWithView
	}
	arenaBytes := int(arenaHdrBytes + c.arenaSize) // bus_arena_hdr_t + usable bytes
	qpBytesLen := qpairBytes(c.slotSize, c.queueCapacity)
	for _, need := range []struct {
		fd  int
		len int
	}{{fds[attachFdControl], ctlLen}, {fds[attachFdArena], arenaBytes}, {fds[attachFdQpair], qpBytesLen}} {
		have, err := fdSize(need.fd)
		if err != nil {
			closeAll()
			return nil, err
		}
		if have < int64(need.len) {
			closeAll()
			return nil, ErrRegion
		}
	}
	ctlMem, err := unix.Mmap(fds[attachFdControl], 0, ctlLen, unix.PROT_READ, unix.MAP_SHARED)
	if err != nil {
		closeAll()
		return nil, err
	}
	arena, err := unix.Mmap(fds[attachFdArena], 0, arenaBytes, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		unix.Munmap(ctlMem)
		closeAll()
		return nil, err
	}
	qpMem, err := unix.Mmap(fds[attachFdQpair], 0, qpBytesLen, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
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
	if c.control.SlotSize() != c.slotSize || c.control.InlineBudget() != c.inlineBudget ||
		c.control.QueueCapacity() != c.queueCapacity || c.control.ArenaSize() != c.arenaSize {
		c.Detach()
		return nil, ErrRegion
	}
	if c.arenaUsable, err = attachArena(arena, c.arenaSize); err != nil {
		c.Detach()
		return nil, err
	}
	if c.qp, err = AttachQueuePair(qpMem); err != nil {
		c.Detach()
		return nil, err
	}
	if c.qp.Inbound.SlotSize() != c.slotSize || c.qp.Inbound.Capacity() != c.queueCapacity ||
		c.qp.Outbound.SlotSize() != c.slotSize || c.qp.Outbound.Capacity() != c.queueCapacity {
		c.Detach()
		return nil, ErrRegion
	}
	c.leaseView = c.control.leaseView()
	return c, nil
}

// ArenaReadable reports whether this attachment can read arena-placed
// payloads (the host advertises the D12 lease view).
func (c *Client) ArenaReadable() bool { return c != nil && c.leaseView != nil }

// FlushReleases returns every owed arena ref the outbound ring has room for.
// Poll does this itself; a consumer that stops polling can call it to drain.
// It returns ErrWouldBlock while any remain.
func (c *Client) FlushReleases() error {
	sent := 0
	for sent < len(c.releases) {
		l := c.releases[sent]
		var p [LeaseReleaseLen]byte
		binary.LittleEndian.PutUint32(p[leaseReleaseOffLease:], l.id)
		binary.LittleEndian.PutUint32(p[leaseReleaseOffGeneration:], l.generation)
		err := c.emit(FNotification|FControl, KindLeaseRelease, 0, p[:])
		if errors.Is(err, ErrWouldBlock) {
			break
		}
		if errors.Is(err, ErrEpoch) {
			return err // the host restarted: every ref died with it
		}
		sent++ // sent, or unsendable: either way not retried
	}
	c.releases = append(c.releases[:0], c.releases[sent:]...)
	if len(c.releases) > 0 {
		return ErrWouldBlock
	}
	return nil
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
	// The outbound ring is SINGLE-PRODUCER: ProduceBegin reads head, the caller
	// writes that slot, and ProduceCommit stores head+1. Two goroutines emitting
	// at once both get the same slot, one overwrites the other, and head
	// advances twice -- so a message is silently lost and a second is delivered
	// twice.
	//
	// That is not hypothetical. A module process serves its stages from its own
	// loop, which emits replies, while anything else holding the client -- a
	// sidecar speaking a second protocol on the one attachment -- emits too. The
	// symptom was applies that left the caller without error and never arrived,
	// which reads as a provider that is up and ignoring writes.
	//
	// Serialised here rather than in each caller, because the invariant belongs
	// to the ring and every current and future emitter has to honour it.
	c.emitMu.Lock()
	defer c.emitMu.Unlock()
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
	// The arena span handed out by the previous Poll is no longer the caller's
	// to read: return its ref (D12).
	if c.pendingLease != nil {
		c.releases = append(c.releases, *c.pendingLease)
		c.pendingLease = nil
	}
	if len(c.releases) > 0 {
		if err := c.FlushReleases(); errors.Is(err, ErrEpoch) {
			return Event{}, false, err
		}
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
	} else if ev.Frame.HdrFlags&FArena != 0 && c.leaseView != nil {
		// Delivering an arena event gave this client a consumer ref, owed back
		// whether or not the payload resolves. With the backlog full, leave the
		// event unread until the outbound ring drains.
		if len(c.releases) >= maxPendingReleases {
			return Event{}, false, nil
		}
		lease := uint32(ev.Frame.PayloadRef)
		if off, ok := resolveLease(c.leaseView, lease, ev.Frame.Generation, ev.Frame.PayloadLen,
			c.arenaSize); ok {
			ev.Payload = c.arenaUsable[off : off+uint64(ev.Frame.PayloadLen)]
		}
		c.pendingLease = &leaseRef{id: lease, generation: ev.Frame.Generation}
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
	align64 := func(n int) int { return (n + qpRingAlign - 1) &^ (qpRingAlign - 1) }
	ring := ringHdrBytes + int(slotSize)*int(capacity)
	inOff := align64(qpBytes)
	outOff := align64(inOff + ring)
	return outOff + ring
}
