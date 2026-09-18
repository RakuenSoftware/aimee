package audit

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
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
