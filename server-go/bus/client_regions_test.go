package bus

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"testing"
	"unsafe"

	"golang.org/x/sys/unix"
)

// These tests run the real attach path (socket, SCM_RIGHTS, fstat, mmap) over
// memfd regions a fake host lays out in Go, so every refusal the client makes
// about the regions it is handed is exercised without the C host.

const (
	fakeSlot  = 256
	fakeInl   = 192
	fakeCap   = 8
	fakeArena = 4096
)

type fakeRegions struct {
	ctl, arena, qp       []byte
	ctlFd, arenaFd, qpFd int
	inOff, outOff        int
}

func memfd(t *testing.T, name string, n int) (int, []byte) {
	t.Helper()
	fd, err := unix.MemfdCreate(name, unix.MFD_CLOEXEC)
	if err != nil {
		t.Skipf("memfd_create: %v", err)
	}
	if err := unix.Ftruncate(fd, int64(n)); err != nil {
		t.Fatal(err)
	}
	mem, err := unix.Mmap(fd, 0, n, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { unix.Munmap(mem); unix.Close(fd) })
	return fd, mem
}

func newFakeRegions(t *testing.T, withView bool) *fakeRegions {
	t.Helper()
	f := &fakeRegions{}
	ctlLen := ctlBytes
	if withView {
		ctlLen = ctlBytesWithView
	}
	f.ctlFd, f.ctl = memfd(t, "ctl", ctlLen)
	copy(f.ctl, validControl(t, withView))
	binary.LittleEndian.PutUint32(f.ctl[ctlOffQueueCap:], fakeCap)
	binary.LittleEndian.PutUint64(f.ctl[ctlOffEpoch:], 1)

	f.arenaFd, f.arena = memfd(t, "arena", arenaHdrBytes+fakeArena)
	binary.LittleEndian.PutUint64(f.arena[arenaOffSize:], fakeArena)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&f.arena[arenaOffMagic])), arenaMagic)

	n := qpairBytes(fakeSlot, fakeCap)
	f.qpFd, f.qp = memfd(t, "qpair", n)
	ring := ringHdrBytes + fakeSlot*fakeCap
	f.inOff = (qpBytes + qpRingAlign - 1) &^ (qpRingAlign - 1)
	f.outOff = (f.inOff + ring + qpRingAlign - 1) &^ (qpRingAlign - 1)
	if _, err := initRing(f.qp[f.inOff:], fakeSlot, fakeCap); err != nil {
		t.Fatal(err)
	}
	if _, err := initRing(f.qp[f.outOff:], fakeSlot, fakeCap); err != nil {
		t.Fatal(err)
	}
	binary.LittleEndian.PutUint32(f.qp[qpOffSlot:], fakeSlot)
	binary.LittleEndian.PutUint32(f.qp[qpOffCap:], fakeCap)
	binary.LittleEndian.PutUint32(f.qp[qpOffInboundOff:], uint32(f.inOff))
	binary.LittleEndian.PutUint32(f.qp[qpOffOutboundOff:], uint32(f.outOff))
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&f.qp[qpOffMagic])), qpMagic)
	return f
}

func (f *fakeRegions) reply() []byte {
	r := make([]byte, attachReplyBytes)
	binary.LittleEndian.PutUint32(r[attachReplyOffMagic:], attachReplyMagic)
	binary.LittleEndian.PutUint32(r[attachReplyOffStatus:], uint32(AttachOK))
	binary.LittleEndian.PutUint32(r[attachReplyOffHandle:], 5)
	binary.LittleEndian.PutUint32(r[attachReplyOffWireVersion:], uint32(WireVersion))
	binary.LittleEndian.PutUint32(r[attachReplyOffSlot:], fakeSlot)
	binary.LittleEndian.PutUint32(r[attachReplyOffInline:], fakeInl)
	binary.LittleEndian.PutUint32(r[attachReplyOffQueueCap:], fakeCap)
	binary.LittleEndian.PutUint64(r[attachReplyOffArena:], fakeArena)
	binary.LittleEndian.PutUint64(r[attachReplyOffEpoch:], 1)
	return r
}

// attachFake runs Attach against a goroutine that answers with reply and the
// given descriptors.
func attachFake(t *testing.T, reply []byte, fds []int) (*Client, error) {
	t.Helper()
	pair, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(pair[0])
	defer unix.Close(pair[1])
	done := make(chan error, 1)
	go func() {
		var req [attachReqBytes]byte
		if _, _, _, _, err := unix.Recvmsg(pair[1], req[:], nil, 0); err != nil {
			done <- err
			return
		}
		var oob []byte
		if len(fds) > 0 {
			oob = unix.UnixRights(fds...)
		}
		done <- unix.Sendmsg(pair[1], reply, oob, nil, 0)
	}()
	c, attachErr := Attach(pair[0])
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if c != nil && attachErr == nil {
		t.Cleanup(c.Detach)
	}
	return c, attachErr
}

func (f *fakeRegions) fds() []int { return []int{f.ctlFd, f.arenaFd, f.qpFd} }

func TestAttachMapsMemfdRegions(t *testing.T) {
	for _, withView := range []bool{true, false} {
		f := newFakeRegions(t, withView)
		c, err := attachFake(t, f.reply(), f.fds())
		if err != nil {
			t.Fatalf("withView=%v: %v", withView, err)
		}
		if c.Handle != 5 || c.ArenaReadable() != withView || len(c.arenaUsable) != fakeArena {
			t.Fatalf("withView=%v: handle %d readable %v arena %d", withView, c.Handle, c.ArenaReadable(), len(c.arenaUsable))
		}
		// The client's view of the regions is the host's: a write through the
		// fake host's mapping is visible to the client.
		f.arena[arenaHdrBytes+7] = 0x5a
		if c.arenaUsable[7] != 0x5a {
			t.Fatal("arena mapping is not shared")
		}
	}
}

func TestAttachRefusesInconsistentRegions(t *testing.T) {
	cases := []struct {
		name string
		poke func(f *fakeRegions, reply []byte)
	}{
		{"arena magic", func(f *fakeRegions, _ []byte) { binary.LittleEndian.PutUint32(f.arena[arenaOffMagic:], 0) }},
		{"arena size below reply", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint64(f.arena[arenaOffSize:], fakeArena-1)
		}},
		{"arena size above reply", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint64(f.arena[arenaOffSize:], fakeArena+1)
		}},
		{"control slot disagrees", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint32(f.ctl[ctlOffSlot:], fakeSlot*2)
		}},
		{"control arena disagrees", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint64(f.ctl[ctlOffArena:], fakeArena*2)
		}},
		{"control capacity disagrees", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint32(f.ctl[ctlOffQueueCap:], fakeCap*2)
		}},
		{"control view advertisement", func(f *fakeRegions, _ []byte) {
			binary.LittleEndian.PutUint32(f.ctl[ctlOffLeaseViewSlots:], 5)
		}},
		{"qpair magic", func(f *fakeRegions, _ []byte) { binary.LittleEndian.PutUint32(f.qp[qpOffMagic:], 0) }},
		{"ring capacity disagrees", func(f *fakeRegions, _ []byte) {
			// A smaller power of two with a matching mask is a valid ring,
			// just not the one the reply announced.
			binary.LittleEndian.PutUint32(f.qp[f.inOff+ringOffCap:], fakeCap/2)
			binary.LittleEndian.PutUint32(f.qp[f.inOff+ringOffMask:], fakeCap/2-1)
		}},
		{"ring magic", func(f *fakeRegions, _ []byte) { binary.LittleEndian.PutUint32(f.qp[f.outOff+ringOffMagic:], 0) }},
	}
	for _, tc := range cases {
		f := newFakeRegions(t, true)
		reply := f.reply()
		tc.poke(f, reply)
		c, err := attachFake(t, reply, f.fds())
		if err == nil {
			c.Detach()
			t.Fatalf("%s: accepted", tc.name)
		}
		if !errors.Is(err, ErrRegion) && !errors.Is(err, ErrRingLayout) {
			t.Fatalf("%s: %v", tc.name, err)
		}
	}
}

// A memfd shorter than its announced geometry would fault on first touch;
// the client must refuse it at attach. Before this check the arena case
// mapped successfully and faulted on the first arena read.
func TestAttachRefusesShortRegions(t *testing.T) {
	for _, which := range []string{"control", "arena", "qpair"} {
		f := newFakeRegions(t, true)
		var fd int
		var n int64
		switch which {
		case "control":
			fd, n = f.ctlFd, ctlBytes-1
		case "arena":
			fd, n = f.arenaFd, arenaHdrBytes+fakeArena-1
		case "qpair":
			fd, n = f.qpFd, int64(qpairBytes(fakeSlot, fakeCap)-1)
		}
		if err := unix.Ftruncate(fd, n); err != nil {
			t.Fatal(err)
		}
		c, err := attachFake(t, f.reply(), f.fds())
		if err == nil {
			c.Detach()
			t.Fatalf("%s: short region accepted", which)
		}
		if !errors.Is(err, ErrRegion) {
			t.Fatalf("%s: %v", which, err)
		}
	}
}

func TestAttachRefusesImpossibleReplies(t *testing.T) {
	cases := []struct {
		name string
		poke func(r []byte)
	}{
		{"wire version", func(r []byte) { binary.LittleEndian.PutUint32(r[attachReplyOffWireVersion:], 2) }},
		{"slot cannot hold a header", func(r []byte) { binary.LittleEndian.PutUint32(r[attachReplyOffSlot:], HdrLen) }},
		{"inline above slot", func(r []byte) { binary.LittleEndian.PutUint32(r[attachReplyOffInline:], fakeSlot+1) }},
		{"zero arena", func(r []byte) { binary.LittleEndian.PutUint64(r[attachReplyOffArena:], 0) }},
		{"huge arena", func(r []byte) { binary.LittleEndian.PutUint64(r[attachReplyOffArena:], ^uint64(0)) }},
	}
	for _, tc := range cases {
		f := newFakeRegions(t, true)
		reply := f.reply()
		tc.poke(reply)
		c, err := attachFake(t, reply, f.fds())
		if err == nil {
			c.Detach()
			t.Fatalf("%s: accepted", tc.name)
		}
		if !errors.Is(err, ErrProtocol) {
			t.Fatalf("%s: %v", tc.name, err)
		}
	}
}

func TestAttachRefusesWrongDescriptorCount(t *testing.T) {
	f := newFakeRegions(t, true)
	for _, fds := range [][]int{nil, {f.ctlFd}, {f.ctlFd, f.arenaFd}} {
		c, err := attachFake(t, f.reply(), fds)
		if err == nil {
			c.Detach()
			t.Fatalf("%d descriptors accepted", len(fds))
		}
		if !errors.Is(err, ErrProtocol) {
			t.Fatalf("%d descriptors: %v", len(fds), err)
		}
	}
}
