package bus

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"testing"
	"unsafe"
)

// D12: the lease view after the control header lets a client in another
// process read an arena payload, and KindLeaseRelease returns the ref. These
// tests build the regions in Go memory, so the client logic is exercised
// without the C host; TestCrossLanguageConformance covers the real host.

func validControl(t *testing.T, withView bool) []byte {
	t.Helper()
	n := ctlBytes
	if withView {
		n = ctlBytesWithView
	}
	mem := make([]byte, n)
	binary.LittleEndian.PutUint32(mem[ctlOffSpec:], specVersion)
	binary.LittleEndian.PutUint32(mem[ctlOffLayout:], layoutVersion)
	binary.LittleEndian.PutUint32(mem[ctlOffSlot:], 256)
	binary.LittleEndian.PutUint32(mem[ctlOffInline:], 192)
	binary.LittleEndian.PutUint32(mem[ctlOffQueueCap:], 4)
	binary.LittleEndian.PutUint64(mem[ctlOffArena:], 4096)
	if withView {
		binary.LittleEndian.PutUint32(mem[ctlOffFlags:], ctlFlagLeaseView)
		binary.LittleEndian.PutUint32(mem[ctlOffLeaseViewSlots:], leaseViewSlots)
		binary.LittleEndian.PutUint64(mem[ctlOffLeaseViewOff:], ctlBytes)
	}
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&mem[ctlOffMagic])), ctlMagic)
	return mem
}

func setEntry(view []byte, lease, generation, length uint32, offset uint64) {
	e := int(lease) * leaseViewEntry
	binary.LittleEndian.PutUint32(view[e+4:], length)
	binary.LittleEndian.PutUint64(view[e+8:], offset)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&view[e])), generation)
}

func TestLeaseViewAdvertisement(t *testing.T) {
	mem := validControl(t, true)
	c, err := AttachControl(mem)
	if err != nil {
		t.Fatalf("valid control with view refused: %v", err)
	}
	if v := c.leaseView(); len(v) != leaseViewSlots*leaseViewEntry {
		t.Fatalf("lease view length = %d", len(v))
	}

	// A header-only mapping (an older host, or a short map) has no view but
	// still attaches.
	hdr, err := AttachControl(mem[:ctlBytes])
	if err != nil {
		t.Fatalf("header-only control refused: %v", err)
	}
	if hdr.leaseView() != nil {
		t.Fatal("a header-only mapping must not expose a view")
	}

	// No flag: no view, even with the bytes mapped.
	plain := validControl(t, false)
	plain = append(plain, make([]byte, ctlBytesWithView-ctlBytes)...)
	pc, err := AttachControl(plain)
	if err != nil || pc.leaseView() != nil {
		t.Fatalf("unflagged control: err=%v view=%v", err, pc.leaseView() != nil)
	}

	// A corrupt advertisement is refused.
	for name, poke := range map[string]func([]byte){
		"slot count": func(m []byte) { binary.LittleEndian.PutUint32(m[ctlOffLeaseViewSlots:], 7) },
		"offset":     func(m []byte) { binary.LittleEndian.PutUint64(m[ctlOffLeaseViewOff:], 64) },
	} {
		bad := validControl(t, true)
		poke(bad)
		if _, err := AttachControl(bad); err == nil {
			t.Fatalf("corrupt view advertisement accepted: %s", name)
		}
	}
}

func TestResolveLease(t *testing.T) {
	view := make([]byte, leaseViewSlots*leaseViewEntry)
	setEntry(view, 5, 3, 256, 128)
	cases := []struct {
		name               string
		lease, gen, length uint32
		arena              uint64
		ok                 bool
		wantOff            uint64
	}{
		{"live", 5, 3, 200, 4096, true, 128},
		{"whole span", 5, 3, 256, 4096, true, 128},
		{"stale generation", 5, 2, 200, 4096, false, 0},
		{"longer than span", 5, 3, 257, 4096, false, 0},
		{"past the arena", 5, 3, 200, 300, false, 0},
		{"id past the table", leaseViewSlots, 3, 1, 4096, false, 0},
		{"generation zero", 5, 0, 1, 4096, false, 0},
		{"empty payload", 5, 3, 0, 4096, false, 0},
		{"empty entry", 6, 1, 1, 4096, false, 0},
	}
	for _, tc := range cases {
		off, ok := resolveLease(view, tc.lease, tc.gen, tc.length, tc.arena)
		if ok != tc.ok || (ok && off != tc.wantOff) {
			t.Fatalf("%s: got (%d, %v), want (%d, %v)", tc.name, off, ok, tc.wantOff, tc.ok)
		}
	}
	setEntry(view, 5, 0, 256, 128) // retired at reclaim
	if _, ok := resolveLease(view, 5, 3, 200, 4096); ok {
		t.Fatal("a retired entry resolved")
	}
	if _, ok := resolveLease(nil, 5, 3, 200, 4096); ok {
		t.Fatal("a missing view resolved")
	}
}

// memClient is a Client over Go-memory rings and regions.
func memClient(t *testing.T, withView bool, capacity uint32) (*Client, *Ring, *Ring, []byte) {
	t.Helper()
	ctl := validControl(t, withView)
	control, err := AttachControl(ctl)
	if err != nil {
		t.Fatal(err)
	}
	inMem := make([]byte, ringHdrBytes+256*int(capacity))
	outMem := make([]byte, ringHdrBytes+256*int(capacity))
	in, err := initRing(inMem, 256, capacity)
	if err != nil {
		t.Fatal(err)
	}
	out, err := initRing(outMem, 256, capacity)
	if err != nil {
		t.Fatal(err)
	}
	arena := make([]byte, 16+4096)
	c := &Client{
		Handle: 3, slotSize: 256, inlineBudget: 192, queueCapacity: capacity, arenaSize: 4096,
		control: control, qp: &QueuePair{mem: make([]byte, qpBytes), Inbound: in, Outbound: out},
		arenaUsable: arena[16:], leaseView: control.leaseView(),
	}
	return c, in, out, arena[16:]
}

func pushArenaFrame(t *testing.T, r *Ring, lease, generation, length uint32) {
	t.Helper()
	slot := r.ProduceBegin()
	if slot == nil {
		t.Fatal("inbound full")
	}
	f := Frame{HdrFlags: FNotification | FArena, WireVersion: WireVersion, EventKind: 300,
		PayloadRef: uint64(lease), PayloadLen: length, Generation: generation, DstHandle: 3}
	if f.Encode(slot) != HdrLen {
		t.Fatal("encode arena frame")
	}
	r.ProduceCommit()
}

func takeRelease(t *testing.T, r *Ring) (uint32, uint32, bool) {
	t.Helper()
	slot := r.ConsumeBegin()
	if slot == nil {
		return 0, 0, false
	}
	defer r.ConsumeCommit()
	var f Frame
	if Decode(slot, &f) != OK {
		t.Fatal("undecodable outbound frame")
	}
	if f.EventKind != KindLeaseRelease || f.HdrFlags&(FNotification|FControl|FInline) != FNotification|FControl|FInline ||
		f.PayloadLen != LeaseReleaseLen {
		t.Fatalf("not a lease release: %+v", f)
	}
	p := slot[f.PayloadRef : f.PayloadRef+uint64(f.PayloadLen)]
	return binary.LittleEndian.Uint32(p), binary.LittleEndian.Uint32(p[4:]), true
}

func TestPollReadsArenaInPlaceAndReleasesOnNextPoll(t *testing.T) {
	c, in, out, arena := memClient(t, true, 4)
	if !c.ArenaReadable() {
		t.Fatal("a view-advertising control must make the client arena-readable")
	}
	for i := 0; i < 600; i++ {
		arena[1024+i] = byte(i*7 + 3)
	}
	setEntry(c.leaseView, 9, 4, 640, 1024)
	pushArenaFrame(t, in, 9, 4, 600)

	ev, ok, err := c.Poll()
	if err != nil || !ok {
		t.Fatalf("poll: ok=%v err=%v", ok, err)
	}
	if len(ev.Payload) != 600 {
		t.Fatalf("arena payload length = %d", len(ev.Payload))
	}
	for i, b := range ev.Payload {
		if b != byte(i*7+3) {
			t.Fatalf("arena byte %d = %d", i, b)
		}
	}
	if _, _, sent := takeRelease(t, out); sent {
		t.Fatal("the ref must not be returned while the caller may still read it")
	}

	if _, ok, _ := c.Poll(); ok {
		t.Fatal("only one event")
	}
	lease, gen, sent := takeRelease(t, out)
	if !sent || lease != 9 || gen != 4 {
		t.Fatalf("release = (%d, %d, %v), want (9, 4, true)", lease, gen, sent)
	}
	if _, _, again := takeRelease(t, out); again {
		t.Fatal("a ref is returned exactly once")
	}
}

func TestUnresolvableArenaFrameStillReturnsItsRef(t *testing.T) {
	c, in, out, _ := memClient(t, true, 4)
	setEntry(c.leaseView, 2, 8, 64, 0)
	pushArenaFrame(t, in, 2, 7, 32) // stale generation
	ev, ok, err := c.Poll()
	if err != nil || !ok || ev.Payload != nil {
		t.Fatalf("stale frame: ok=%v err=%v payload=%v", ok, err, ev.Payload != nil)
	}
	c.Poll()
	if lease, gen, sent := takeRelease(t, out); !sent || lease != 2 || gen != 7 {
		t.Fatalf("release = (%d, %d, %v)", lease, gen, sent)
	}
}

func TestOlderHostLeavesArenaUnreadAndUnreleased(t *testing.T) {
	c, in, out, _ := memClient(t, false, 4)
	if c.ArenaReadable() {
		t.Fatal("no view: not arena-readable")
	}
	pushArenaFrame(t, in, 1, 1, 32)
	ev, ok, _ := c.Poll()
	if !ok || ev.Payload != nil {
		t.Fatalf("older host: ok=%v payload=%v", ok, ev.Payload != nil)
	}
	c.Poll()
	if _, _, sent := takeRelease(t, out); sent {
		t.Fatal("a release must not be sent to a host that cannot consume it")
	}
}

// The real path: refs that cannot leave (outbound full) queue to the bound,
// then arena frames wait unread; draining the ring resumes every frame in
// order and returns every ref.
func TestReleaseBacklogFillsThroughPollAndDrains(t *testing.T) {
	n := maxPendingReleases + 10
	c, in, out, arena := memClient(t, true, 128)
	for i := 0; i < n; i++ {
		arena[i*32] = byte(i)
		setEntry(c.leaseView, uint32(i), 1, 32, uint64(i*32))
		pushArenaFrame(t, in, uint32(i), 1, 32)
	}
	for out.ProduceBegin() != nil {
		out.ProduceCommit()
	}
	got := 0
	for {
		ev, ok, err := c.Poll()
		if err != nil {
			t.Fatal(err)
		}
		if !ok {
			break
		}
		if ev.Payload[0] != byte(got) {
			t.Fatalf("event %d out of order", got)
		}
		got++
	}
	if got != maxPendingReleases || len(c.releases) != maxPendingReleases {
		t.Fatalf("read %d, backlog %d; want %d each", got, len(c.releases), maxPendingReleases)
	}
	if int(in.Count()) != n-got {
		t.Fatalf("unread %d, want %d", in.Count(), n-got)
	}
	for out.ConsumeBegin() != nil {
		out.ConsumeCommit()
	}
	for {
		ev, ok, err := c.Poll()
		if err != nil {
			t.Fatal(err)
		}
		if !ok {
			break
		}
		if ev.Payload[0] != byte(got) {
			t.Fatalf("event %d out of order after drain", got)
		}
		got++
	}
	if got != n {
		t.Fatalf("read %d of %d", got, n)
	}
	if err := c.FlushReleases(); err != nil {
		t.Fatalf("flush: %v", err)
	}
	seen := map[uint32]bool{}
	for {
		lease, gen, ok := takeRelease(t, out)
		if !ok {
			break
		}
		if gen != 1 || seen[lease] {
			t.Fatalf("release (%d,%d) wrong or repeated", lease, gen)
		}
		seen[lease] = true
	}
	if len(seen) != n {
		t.Fatalf("%d refs returned, want %d", len(seen), n)
	}
}

func TestReleaseBacklogAppliesBackpressure(t *testing.T) {
	c, in, out, _ := memClient(t, true, 2)
	// Fill the outbound ring so no release can be sent.
	for out.ProduceBegin() != nil {
		out.ProduceCommit()
	}
	c.releases = make([]leaseRef, maxPendingReleases)
	setEntry(c.leaseView, 1, 1, 64, 0)
	pushArenaFrame(t, in, 1, 1, 32)
	if _, ok, err := c.Poll(); ok || err != nil {
		t.Fatalf("a full backlog must leave the arena event unread: ok=%v err=%v", ok, err)
	}
	if in.Count() != 1 {
		t.Fatal("the event must stay on the inbound ring")
	}
	if err := c.FlushReleases(); !errors.Is(err, ErrWouldBlock) {
		t.Fatalf("flush with a full ring = %v, want ErrWouldBlock", err)
	}
	// Drain the outbound ring: the backlog flushes and the event is taken.
	for out.ConsumeBegin() != nil {
		out.ConsumeCommit()
	}
	c.releases = c.releases[:1]
	ev, ok, err := c.Poll()
	if err != nil || !ok || len(ev.Payload) != 32 {
		t.Fatalf("after drain: ok=%v err=%v len=%d", ok, err, len(ev.Payload))
	}
	if len(c.releases) != 0 {
		t.Fatalf("backlog not flushed: %d", len(c.releases))
	}
}

// The host routes arena requests to a kind's server and arena replies back
// (route_arena_request, route_arena_reply), so a module process can receive
// either. The client must read and release them as it does notifications.
func TestArenaRequestsAndRepliesResolveAndRelease(t *testing.T) {
	for i, pattern := range []uint16{FRequest, FReply} {
		c, in, out, arena := memClient(t, true, 4)
		for j := 0; j < 300; j++ {
			arena[2048+j] = byte(j + i)
		}
		setEntry(c.leaseView, 11, 3, 512, 2048)
		slot := in.ProduceBegin()
		f := Frame{HdrFlags: pattern | FArena, WireVersion: WireVersion, EventKind: 300,
			CorrelationID: 70 + uint64(i), PayloadRef: 11, PayloadLen: 300, Generation: 3, DstHandle: 3}
		if f.Encode(slot) != HdrLen {
			t.Fatal("encode")
		}
		in.ProduceCommit()
		ev, ok, err := c.Poll()
		if err != nil || !ok || len(ev.Payload) != 300 || ev.Payload[299] != byte(299+i) {
			t.Fatalf("pattern %#x: ok=%v err=%v len=%d", pattern, ok, err, len(ev.Payload))
		}
		if ev.Frame.CorrelationID != 70+uint64(i) || ev.Frame.HdrFlags&(FRequest|FReply) != pattern {
			t.Fatalf("pattern %#x: frame %+v", pattern, ev.Frame)
		}
		c.Poll()
		if lease, gen, sent := takeRelease(t, out); !sent || lease != 11 || gen != 3 {
			t.Fatalf("pattern %#x: release = (%d, %d, %v)", pattern, lease, gen, sent)
		}
	}
}
