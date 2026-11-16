package ir

import (
	"bytes"
	"encoding/json"
	"os/exec"
	"testing"
)

func TestUsagePresenceCGoConformance(t *testing.T) {
	bridge := cResponseBridge(t)
	for _, usage := range []ResponseUsage{ZeroUsage(), {Unobserved: true}} {
		response := TextResponse("answer")
		response.Usage = usage
		wire, err := json.Marshal(response)
		if err != nil {
			t.Fatal(err)
		}
		command := exec.Command(bridge, "parse")
		command.Stdin = bytes.NewReader(wire)
		recoded, err := command.Output()
		if err != nil {
			t.Fatalf("C rejects usage: %v %s", err, wire)
		}
		decoded, err := DecodeResponse(recoded)
		if err != nil || decoded.Usage != usage {
			t.Fatalf("usage presence lost: %+v %v", decoded.Usage, err)
		}
	}
	for _, bad := range []string{`{}`, `{"unobserved":false}`, `{"unobserved":null}`, `{"unobserved":true,"input":"0"}`, `{"unobserved":true,"unobserved":true}`, `{"input":"0","output":"0","cache_read":"0","cache_write":"0","reasoning":null}`} {
		wire, _ := json.Marshal(TextResponse("answer"))
		var fields map[string]json.RawMessage
		if err := json.Unmarshal(wire, &fields); err != nil {
			t.Fatal(err)
		}
		fields["usage"] = json.RawMessage(bad)
		wire, _ = json.Marshal(fields)
		if _, err := DecodeResponse(wire); err == nil {
			t.Fatalf("Go admitted %s", bad)
		}
		command := exec.Command(bridge, "parse")
		command.Stdin = bytes.NewReader(wire)
		if err := command.Run(); err == nil {
			t.Fatalf("C admitted %s", bad)
		}
	}
}
