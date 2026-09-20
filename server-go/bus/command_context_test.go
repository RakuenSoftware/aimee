package bus

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"strings"
	"testing"
)

func TestCommandContextWire(t *testing.T) {
	caller := CommandContext{Authenticated: true, Principal: "user:alice", TransportIdentity: "cert:server", UserAuthority: true, ScopeKind: "project", ScopeID: "app"}
	frame, err := EncodeCommandWithContext("delete", json.RawMessage(`{"id":7,"principal":"forged"}`), caller)
	if err != nil {
		t.Fatal(err)
	}
	if string(frame[:4]) != "CMPQ" || binary.LittleEndian.Uint32(frame[4:]) != 2 || binary.LittleEndian.Uint16(frame[8:]) != 6 || string(frame[20:26]) != "delete" {
		t.Fatalf("%x", frame)
	}
	verb, args, got, err := DecodeCommandWithContext(frame)
	if err != nil || verb != "delete" || string(args) != `{"id":7,"principal":"forged"}` || got == nil || *got != caller {
		t.Fatalf("%s %s %+v %v", verb, args, got, err)
	}
	if _, _, err := DecodeCommand(frame); err == nil {
		t.Fatal("legacy decoder accepted contextual frame")
	}
	old, _ := EncodeCommand("get", nil)
	if _, _, got, err := DecodeCommandWithContext(old); err != nil || got != nil {
		t.Fatal(got, err)
	}
	for n := 0; n < len(frame); n++ {
		if _, _, _, err := DecodeCommandWithContext(frame[:n]); err == nil {
			t.Fatalf("accepted prefix %d", n)
		}
	}
	for _, mutate := range []func([]byte){
		func(b []byte) { b[0]++ }, func(b []byte) { b[4] = 3 }, func(b []byte) { b[10] = 1 },
		func(b []byte) { binary.LittleEndian.PutUint32(b[12:], ^uint32(0)) },
		func(b []byte) { binary.LittleEndian.PutUint32(b[16:], 4097) }, func(b []byte) { b[len(b)-1] = '!' },
	} {
		bad := bytes.Clone(frame)
		mutate(bad)
		if _, _, _, err := DecodeCommandWithContext(bad); err == nil {
			t.Fatalf("accepted %x", bad)
		}
	}
	for _, c := range []CommandContext{{UserAuthority: true}, {Authenticated: true}, {Principal: strings.Repeat("x", 577)}, {Principal: "user\x00forged"}, {ScopeID: "app"}, {ScopeKind: "project"}, {Authenticated: true, Principal: "alice", ScopeKind: strings.Repeat("x", 65)}, {Authenticated: true, Principal: "alice", ScopeKind: "project", ScopeID: "app\x00forged"}} {
		if _, err := EncodeCommandWithContext("get", nil, c); err == nil {
			t.Fatal(c)
		}
	}
	// Independent bytes, as emitted by the native host: context follows args.
	raw := []byte(`{"authenticated":true,"principal":"user:alice"}`)
	frozen := append([]byte{'C', 'M', 'P', 'Q', 2, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, byte(len(raw)), 0, 0, 0, 'g', 'e', 't', '{', '}'}, raw...)
	if verb, args, c, err := DecodeCommandWithContext(frozen); err != nil || verb != "get" || string(args) != "{}" || c.Principal != "user:alice" || !c.Authenticated || c.UserAuthority {
		t.Fatalf("%s %s %+v %v", verb, args, c, err)
	}
}
