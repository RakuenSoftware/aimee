package bus

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"testing"
)

func TestCommandWire(t *testing.T) {
	frame, err := EncodeCommand("get", json.RawMessage(`{"id":42}`))
	if err != nil {
		t.Fatal(err)
	}
	// Frozen frame shared with the native command dispatcher.
	want := []byte{'C', 'M', 'P', 'Q', 1, 0, 0, 0, 3, 0, 0, 0, 9, 0, 0, 0, 'g', 'e', 't', '{', '"', 'i', 'd', '"', ':', '4', '2', '}'}
	if !bytes.Equal(frame, want) {
		t.Fatalf("frame=%x want=%x", frame, want)
	}
	verb, args, err := DecodeCommand(want)
	if err != nil || verb != "get" || string(args) != `{"id":42}` {
		t.Fatalf("%s %s %v", verb, args, err)
	}
	result, err := EncodeCommandResult(json.RawMessage(`{"status":"ok"}`))
	if err != nil {
		t.Fatal(err)
	}
	body, err := DecodeCommandResult(result)
	if err != nil || string(body) != `{"status":"ok"}` {
		t.Fatalf("%s %v", body, err)
	}
	for _, mutate := range []func([]byte){
		func(b []byte) { b[0]++ }, func(b []byte) { b[4]++ },
		func(b []byte) { b[10] = 1 }, func(b []byte) { b[8] = 0 },
		func(b []byte) { binary.LittleEndian.PutUint32(b[12:16], ^uint32(0)) },
		func(b []byte) { b[len(b)-1] = '!' },
	} {
		bad := bytes.Clone(frame)
		mutate(bad)
		if _, _, err := DecodeCommand(bad); err == nil {
			t.Fatalf("accepted malformed frame %x", bad)
		}
	}
	for n := 0; n < len(frame); n++ {
		if _, _, err := DecodeCommand(frame[:n]); err == nil {
			t.Fatalf("accepted truncated request %d", n)
		}
	}
	for n := 0; n < len(result); n++ {
		if _, err := DecodeCommandResult(result[:n]); err == nil {
			t.Fatalf("accepted truncated response %d", n)
		}
	}
}
