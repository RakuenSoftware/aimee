package audit

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestActionNativeWire(t *testing.T) {
	// Seven little-endian length-prefixed strings, followed by signed int64.
	// This fixed vector can be consumed directly by obs_bus.c::write_row.
	got := EncodeAction(Action{Actor: "a", Tool: "b", TaskID: 0x0102030405060708})
	want, _ := hex.DecodeString("0100000061010000006200000000000000000000000000000000000000000807060504030201")
	if !bytes.Equal(got, want) {
		t.Fatalf("wire=%x want=%x", got, want)
	}
	long := strings.Repeat("界", 512)
	body := EncodeAction(Action{Actor: long, Tool: "tool\n\x1b\x7f", ArgsHash: long, Command: long, Mode: long, Reason: long, Verdict: long, TaskID: -1})
	reader := bytes.NewReader(body)
	for i, cap := range []uint32{128, 256, 96, 512, 64, 128, 32} {
		var size uint32
		if err := binary.Read(reader, binary.LittleEndian, &size); err != nil || size >= cap {
			t.Fatal(i, size, err)
		}
		value := make([]byte, size)
		if _, err := reader.Read(value); err != nil {
			t.Fatal(err)
		}
		if !utf8.Valid(value) {
			t.Fatal("split UTF-8", i)
		}
		if i == 1 && string(value) != "tool???" {
			t.Fatal(string(value))
		}
	}
	var id int64
	if err := binary.Read(reader, binary.LittleEndian, &id); err != nil || id != -1 || reader.Len() != 0 {
		t.Fatal(id, reader.Len(), err)
	}
}

type testPublisher struct {
	attempts int
	failure  error
	bodies   [][]byte
}

func (p *testPublisher) Publish(kind uint32, body []byte) error {
	if kind != ActionKind {
		panic("wrong action kind")
	}
	p.attempts++
	if p.failure != nil {
		return p.failure
	}
	if p.attempts < 3 {
		return bus.ErrWouldBlock
	}
	p.bodies = append(p.bodies, append([]byte(nil), body...))
	return nil
}

func TestActionBackpressure(t *testing.T) {
	p := &testPublisher{}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	action := Action{Tool: "fixture", TaskID: 42}
	if err := PublishAction(ctx, p, action); err != nil || p.attempts != 3 || len(p.bodies) != 1 || !bytes.Equal(p.bodies[0], EncodeAction(action)) {
		t.Fatal(p, err)
	}
	failure := errors.New("disconnected")
	p = &testPublisher{failure: failure}
	if err := PublishAction(ctx, p, action); !errors.Is(err, failure) || p.attempts != 1 {
		t.Fatal(p, err)
	}
	cancelled, stop := context.WithCancel(context.Background())
	stop()
	p = &testPublisher{}
	if err := PublishAction(cancelled, p, action); !errors.Is(err, context.Canceled) || p.attempts != 0 {
		t.Fatal(p, err)
	}
	deadline, stop := context.WithTimeout(context.Background(), 5*time.Millisecond)
	defer stop()
	p = &testPublisher{failure: bus.ErrWouldBlock}
	if err := PublishAction(deadline, p, action); !errors.Is(err, context.DeadlineExceeded) || p.attempts == 0 {
		t.Fatal(p, err)
	}
}

// The native fixture hosts obs_bus and reads the real ledger. It admits this
// test executable only for ACTION publication, and acknowledges ledger arrival.
func TestActionNativeLedger(t *testing.T) {
	fixture := os.Getenv("AIMEE_AUDIT_LEDGER_FIXTURE")
	if fixture == "" {
		t.Skip("set AIMEE_AUDIT_LEDGER_FIXTURE to the native audit fixture")
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, fixture, "--go-publisher", executable).CombinedOutput()
	if err != nil || !bytes.Contains(output, []byte("Go ACTION publisher -> authenticated daemon bus -> ledger: ok")) {
		t.Fatalf("%s: %v", output, err)
	}
}

func TestActionPublisherProcess(t *testing.T) {
	socket := os.Getenv("AIMEE_AUDIT_FIXTURE_SOCKET")
	if socket == "" {
		t.Skip("native fixture child")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	client, err := bus.ConnectClient(ctx, socket, 1, 73)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Detach()
	if err = PublishAction(ctx, client, Action{Actor: "go\nproducer", Tool: "go.action.fixture", ArgsHash: "v1-", Command: "mk:0123456789ab", Mode: "L2", Reason: "conf=0.88", Verdict: "ok", TaskID: 101}); err != nil {
		t.Fatal(err)
	}
	for {
		if _, err = os.Stat(os.Getenv("AIMEE_AUDIT_FIXTURE_ACK")); err == nil {
			return
		}
		select {
		case <-ctx.Done():
			t.Fatal("ledger did not acknowledge action")
		case <-time.After(time.Millisecond * 10):
		}
	}
}
