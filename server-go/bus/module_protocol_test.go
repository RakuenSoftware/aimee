package bus

import "testing"

func TestModuleMessageRoundTrip(t *testing.T) {
	want := ModuleMessage{Operation: ModuleOpInvoke, StageID: 0x1701, BodyLen: 3, DeadlineNS: 900, TraceID: 42}
	wire := make([]byte, ModuleMessageHeaderLen+3)
	if _, err := want.Encode(wire); err != nil {
		t.Fatal(err)
	}
	copy(wire[ModuleMessageHeaderLen:], "abc")
	got, err := DecodeModuleMessage(wire)
	if err != nil || got != want {
		t.Fatalf("got %#v, %v; want %#v", got, err, want)
	}
	if got.DeadlineExpired(899) || !got.DeadlineExpired(900) {
		t.Fatal("deadline boundary mismatch")
	}
}

func TestModuleMessageRejectsTruncatedBody(t *testing.T) {
	m := ModuleMessage{Operation: ModuleOpResult, Status: ModuleStatusCapabilityAbsent, StageID: 7, BodyLen: 1}
	wire := make([]byte, ModuleMessageHeaderLen)
	if _, err := m.Encode(wire); err != nil {
		t.Fatal(err)
	}
	if _, err := DecodeModuleMessage(wire); err == nil {
		t.Fatal("accepted truncated body")
	}
}
