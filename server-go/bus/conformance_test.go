package bus

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

// TestCrossLanguageConformance connects a real Go client to the in-source C host
// (built as a test harness) over a Unix socket, and exercises both directions of
// the wire. Two independent client implementations — this Go one and the C one
// inside the harness — on one host is the credibility test for "any language".
//
// The harness binary path is passed in BUS_CONFORMANCE_HOST; without it the test
// skips, so `go test ./bus/...` alone stays hermetic. scripts/test_bus_conformance.sh
// builds the harness and sets the variable.
func TestCrossLanguageConformance(t *testing.T) {
	harness := os.Getenv("BUS_CONFORMANCE_HOST")
	if harness == "" {
		t.Skip("BUS_CONFORMANCE_HOST not set; run via scripts/test_bus_conformance.sh")
	}

	dir := t.TempDir()
	sock := filepath.Join(dir, "bus.sock")

	cmd := exec.Command(harness, sock, "15000")
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	if err := cmd.Start(); err != nil {
		t.Fatalf("start harness: %v", err)
	}
	defer func() {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		if t.Failed() {
			t.Logf("harness output:\n%s", out.String())
		}
	}()

	// Wait for the harness to create the listening socket.
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(sock); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("harness socket did not appear")
		}
		time.Sleep(10 * time.Millisecond)
	}

	// Connect a real Go client over SOCK_SEQPACKET.
	fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET, 0)
	if err != nil {
		t.Fatalf("socket: %v", err)
	}
	if err := unix.Connect(fd, &unix.SockaddrUnix{Name: sock}); err != nil {
		unix.Close(fd)
		t.Fatalf("connect: %v", err)
	}
	// Closed explicitly at step 5 (reaped-recovery); guard the defer against a
	// double close.
	fdClosed := false
	defer func() {
		if !fdClosed {
			unix.Close(fd)
		}
	}()

	c, err := Attach(fd)
	if err != nil {
		t.Fatalf("attach: %v", err)
	}
	defer c.Detach()
	t.Logf("attached: handle=%d slot=%d cap=%d", c.Handle, c.slotSize, c.queueCapacity)

	// pollFor waits up to a deadline for an event matching pred.
	pollFor := func(pred func(Event) bool, what string) Event {
		t.Helper()
		dl := time.Now().Add(5 * time.Second)
		for time.Now().Before(dl) {
			ev, ok, err := c.Poll()
			if err != nil {
				t.Fatalf("poll (%s): %v", what, err)
			}
			if ok && pred(ev) {
				return ev
			}
			if !ok {
				time.Sleep(2 * time.Millisecond)
			}
		}
		t.Fatalf("timed out waiting for %s", what)
		return Event{}
	}

	// 1. Go -> C -> Go: publish a notification the C client acks.
	payload := []byte("go-says-hi")
	if err := c.Publish(KIND_NOTIFY, payload); err != nil {
		t.Fatalf("publish notify: %v", err)
	}
	ack := pollFor(func(ev Event) bool { return ev.Frame.EventKind == KIND_ACK }, "ack")
	if !bytes.Equal(ack.Payload, payload) {
		t.Fatalf("ack payload = %q, want %q", ack.Payload, payload)
	}
	t.Log("notification round-trip Go->C->Go ok")

	// 2. Go <-> C: an echo request/reply across the language boundary.
	req := []byte("echo-me")
	if err := c.Request(KIND_ECHO, 0xABCDEF, req); err != nil {
		t.Fatalf("request echo: %v", err)
	}
	reply := pollFor(func(ev Event) bool {
		return ev.Frame.EventKind == KIND_ECHO && ev.Frame.HdrFlags&FReply != 0
	}, "echo reply")
	if reply.Frame.CorrelationID != 0xABCDEF || !bytes.Equal(reply.Payload, req) {
		t.Fatalf("echo reply mismatch: corr=0x%x payload=%q", reply.Frame.CorrelationID, reply.Payload)
	}
	t.Log("echo request/reply Go<->C ok")

	// 3. A request for a kind with no server draws capability_absent.
	if err := c.Request(KIND_NOSERVER, 0x999, nil); err != nil {
		t.Fatalf("request noserver: %v", err)
	}
	ca := pollFor(func(ev Event) bool {
		return ev.Frame.EventKind == KindCapabilityAbsent
	}, "capability_absent")
	if ca.Frame.CorrelationID != 0x999 || ca.Frame.HdrFlags&FReply == 0 {
		t.Fatalf("capability_absent mismatch: corr=0x%x flags=0x%x",
			ca.Frame.CorrelationID, ca.Frame.HdrFlags)
	}
	t.Log("capability_absent ok")

	// 4. Cancel: the request reaches the C server, which acks the cancel back so
	// its delivery is observable across the boundary.
	if err := c.Request(KIND_ECHO, 0x5151, []byte("to-cancel")); err != nil {
		t.Fatalf("request to cancel: %v", err)
	}
	// The reply for this request may arrive; drain whatever comes, then cancel.
	if err := c.Cancel(KIND_ECHO, 0x5151); err != nil {
		t.Fatalf("cancel: %v", err)
	}
	pollFor(func(ev Event) bool { return ev.Frame.EventKind == KIND_CANCEL_ACK }, "cancel ack")
	t.Log("cancel delivered to the server ok")

	// 4b. Arena across the process boundary (D12): the C side leases and fills
	// an arena span and publishes it; this Go client reads it through the lease
	// view, returns the ref on its next poll, and the C side confirms the lease
	// drained.
	if !c.ArenaReadable() {
		t.Fatal("the C host must advertise the lease view")
	}
	const arenaLen = 5000
	var ask [4]byte
	binary.LittleEndian.PutUint32(ask[:], arenaLen)
	if err := c.Request(KIND_ARENA_ASK, 0xA1, ask[:]); err != nil {
		t.Fatalf("request arena: %v", err)
	}
	var data Event
	sawData, sawReply := false, false
	for !(sawData && sawReply) {
		ev := pollFor(func(ev Event) bool {
			return ev.Frame.EventKind == KIND_ARENA_DATA || ev.Frame.EventKind == KIND_ARENA_ASK
		}, "arena data and ask reply")
		if ev.Frame.EventKind == KIND_ARENA_ASK {
			if len(ev.Payload) != 1 || ev.Payload[0] != 0 {
				t.Fatalf("arena ask refused by the C side: %v", ev.Payload)
			}
			sawReply = true
			continue
		}
		data = ev
		if data.Frame.HdrFlags&FArena == 0 || len(data.Payload) != arenaLen {
			t.Fatalf("arena data: flags=0x%x len=%d", data.Frame.HdrFlags, len(data.Payload))
		}
		for i, b := range data.Payload {
			if b != byte(i*7+3) {
				t.Fatalf("arena byte %d = %d", i, b)
			}
		}
		sawData = true
	}
	// The next poll returns the ref; then ask the C side what is still live.
	if _, _, err := c.Poll(); err != nil {
		t.Fatalf("poll to release: %v", err)
	}
	if err := c.Request(KIND_ARENA_STATS, 0xA2, nil); err != nil {
		t.Fatalf("request stats: %v", err)
	}
	stats := pollFor(func(ev Event) bool {
		return ev.Frame.EventKind == KIND_ARENA_STATS && ev.Frame.HdrFlags&FReply != 0
	}, "arena stats")
	if len(stats.Payload) != 4 || binary.LittleEndian.Uint32(stats.Payload) != 0 {
		t.Fatalf("lease still live after release: %v", stats.Payload)
	}
	t.Log("arena payload read across the process boundary and released ok")

	// 4c. Arena-placed requests and replies: this client serves an arena
	// REQUEST the C side sends it, and reads an arena REPLY to its own request.
	patternOK := func(p []byte, n int) bool {
		if len(p) != n {
			return false
		}
		for i, b := range p {
			if b != byte(i*7+3) {
				return false
			}
		}
		return true
	}
	noLiveLease := func(corr uint64, what string) {
		if _, _, err := c.Poll(); err != nil { // returns the last arena ref
			t.Fatalf("%s: poll: %v", what, err)
		}
		if err := c.Request(KIND_ARENA_STATS, corr, nil); err != nil {
			t.Fatalf("%s: stats: %v", what, err)
		}
		st := pollFor(func(ev Event) bool {
			return ev.Frame.EventKind == KIND_ARENA_STATS && ev.Frame.HdrFlags&FReply != 0
		}, "arena stats")
		if len(st.Payload) != 4 || binary.LittleEndian.Uint32(st.Payload) != 0 {
			t.Fatalf("%s: lease still live: %v", what, st.Payload)
		}
	}
	for _, n := range []uint32{193, 60000} {
		var ask [4]byte
		binary.LittleEndian.PutUint32(ask[:], n)
		if err := c.Request(KIND_ARENA_SERVE_ASK, 0xA300+uint64(n), ask[:]); err != nil {
			t.Fatalf("serve ask: %v", err)
		}
		req := pollFor(func(ev Event) bool {
			return ev.Frame.EventKind == KIND_ARENA_SERVE && ev.Frame.HdrFlags&FRequest != 0
		}, "arena request")
		if req.Frame.HdrFlags&FArena == 0 {
			t.Fatal("the request is not arena-placed")
		}
		verdict := byte(0)
		if patternOK(req.Payload, int(n)) {
			verdict = 1
		}
		if err := c.Reply(KIND_ARENA_SERVE, req.Frame.CorrelationID, []byte{verdict}); err != nil {
			t.Fatalf("reply to arena request: %v", err)
		}
		ans := pollFor(func(ev Event) bool {
			return ev.Frame.EventKind == KIND_ARENA_SERVE_ASK && ev.Frame.HdrFlags&FReply != 0
		}, "serve verdict")
		if ans.Frame.CorrelationID != 0xA300+uint64(n) || len(ans.Payload) != 1 || ans.Payload[0] != 1 {
			t.Fatalf("len %d: served arena request verdict %v", n, ans.Payload)
		}
		noLiveLease(0xA400+uint64(n), "served request")

		binary.LittleEndian.PutUint32(ask[:], n)
		if err := c.Request(KIND_ARENA_REPLY_ASK, 0xA500+uint64(n), ask[:]); err != nil {
			t.Fatalf("reply ask: %v", err)
		}
		rep := pollFor(func(ev Event) bool {
			return ev.Frame.EventKind == KIND_ARENA_REPLY_ASK && ev.Frame.HdrFlags&FReply != 0
		}, "arena reply")
		if rep.Frame.CorrelationID != 0xA500+uint64(n) || rep.Frame.HdrFlags&FArena == 0 ||
			!patternOK(rep.Payload, int(n)) {
			t.Fatalf("len %d: arena reply flags=0x%x len=%d", n, rep.Frame.HdrFlags, len(rep.Payload))
		}
		noLiveLease(0xA600+uint64(n), "arena reply")
	}
	t.Log("arena requests and replies served and read across the process boundary ok")

	// 5. Reaped-client recovery: this Go client detaches; the host reaps it and
	// frees its slot. A fresh Go client then attaches into that slot and works —
	// proving recovery across the language boundary.
	c.Detach()
	unix.Close(fd)
	fdClosed = true

	fd2, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET, 0)
	if err != nil {
		t.Fatalf("socket 2: %v", err)
	}
	defer unix.Close(fd2)
	// The harness reaps on disconnect then re-accepts; retry connect briefly.
	connected := false
	for i := 0; i < 200; i++ {
		if err := unix.Connect(fd2, &unix.SockaddrUnix{Name: sock}); err == nil {
			connected = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !connected {
		t.Fatal("second client could not connect after recovery")
	}
	c2, err := Attach(fd2)
	if err != nil {
		t.Fatalf("second attach (recovery): %v", err)
	}
	defer c2.Detach()

	poll2 := func(pred func(Event) bool, what string) Event {
		t.Helper()
		dl := time.Now().Add(5 * time.Second)
		for time.Now().Before(dl) {
			ev, ok, err := c2.Poll()
			if err != nil {
				t.Fatalf("poll2 (%s): %v", what, err)
			}
			if ok && pred(ev) {
				return ev
			}
			if !ok {
				time.Sleep(2 * time.Millisecond)
			}
		}
		t.Fatalf("recovered client timed out waiting for %s", what)
		return Event{}
	}
	if err := c2.Request(KIND_ECHO, 0x7777, []byte("after-recovery")); err != nil {
		t.Fatalf("recovered request: %v", err)
	}
	r2 := poll2(func(ev Event) bool {
		return ev.Frame.EventKind == KIND_ECHO && ev.Frame.HdrFlags&FReply != 0
	}, "recovered echo reply")
	if r2.Frame.CorrelationID != 0x7777 || !bytes.Equal(r2.Payload, []byte("after-recovery")) {
		t.Fatalf("recovered reply mismatch")
	}
	t.Log("reaped-client recovery ok: a fresh client works after the first was reaped")
}

const (
	// Kinds must match bus_conformance_host.c.
	KIND_NOTIFY      uint32 = 1000
	KIND_ACK         uint32 = 1001
	KIND_ECHO        uint32 = 1002
	KIND_NOSERVER    uint32 = 1003
	KIND_CANCEL_ACK  uint32 = 1004
	KIND_ARENA_ASK   uint32 = 1005
	KIND_ARENA_DATA  uint32 = 1006
	KIND_ARENA_STATS uint32 = 1007

	KIND_ARENA_SERVE_ASK uint32 = 1008
	KIND_ARENA_SERVE     uint32 = 1009
	KIND_ARENA_REPLY_ASK uint32 = 1010
)
