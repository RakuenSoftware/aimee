package bus

import (
	"bytes"
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
	KIND_NOTIFY     uint32 = 1000
	KIND_ACK        uint32 = 1001
	KIND_ECHO       uint32 = 1002
	KIND_NOSERVER   uint32 = 1003
	KIND_CANCEL_ACK uint32 = 1004
)
