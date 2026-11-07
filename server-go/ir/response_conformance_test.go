package ir

import (
	"bytes"
	"encoding/json"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func cResponseBridge(t *testing.T) string {
	t.Helper()
	source := filepath.Clean("../../src")
	binary := filepath.Join(t.TempDir(), "response-bridge")
	command := exec.Command("cc", "-std=c11", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
		"-I"+source, "-I"+filepath.Join(source, "vendor/headers"), "-I"+filepath.Join(source, "modules/ir/include"),
		"testdata/response_bridge.c", filepath.Join(source, "modules/ir/aimee_ir.c"), filepath.Join(source, "modules/ir/session_event.c"), filepath.Join(source, "modules/ir/response_codec.c"), filepath.Join(source, "modules/ir/request_codec.c"), filepath.Join(source, "vendor/cJSON.c"), "-lm", "-o", binary)
	if out, err := command.CombinedOutput(); err != nil {
		t.Fatalf("production C response bridge: %v\n%s", err, out)
	}
	return binary
}
func jsonValue(t *testing.T, raw []byte) any {
	t.Helper()
	var value any
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber()
	if err := d.Decode(&value); err != nil {
		t.Fatal(err)
	}
	return value
}
func TestResponseCCodecConformance(t *testing.T) {
	bridge := cResponseBridge(t)
	raw, err := exec.Command(bridge).Output()
	if err != nil {
		t.Fatal(err)
	}
	response, err := DecodeResponse(raw)
	if err != nil {
		t.Fatalf("rejects actual C response: %v\n%s", err, raw)
	}
	if response.Refusal == nil || response.Refusal.Explanation != nil || len(response.Blocks) != 5 || response.Usage.Input != "9223372036854775807" || response.Blocks[0].Context.RevisionEpoch != "18446744073709551615" {
		t.Fatalf("lost typed fields: %+v", response)
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(bridge, "parse")
	command.Stdin = bytes.NewReader(encoded)
	recoded, err := command.Output()
	if err != nil {
		t.Fatalf("C rejects Go wire: %v\n%s", err, encoded)
	}
	if !reflect.DeepEqual(jsonValue(t, raw), jsonValue(t, recoded)) {
		t.Fatalf("C→Go→C changed canonical typed/opaque data:\n%s\n%s", raw, recoded)
	}
	// Actual C text projection preserves split JSON tokens and authored groups.
	for _, groups := range [][]int{{0, 0}, {1, 1}, {1, 2}} {
		left, right := `{"id":"ab`, `cd"}`
		r := TextResponse(left)
		r.Blocks[0].MessageGroup = &groups[0]
		r.Blocks = append(r.Blocks, ResponseBlock{Block: Block{Type: "text", Text: &right, MessageGroup: &groups[1]}})
		if groups[0] == 0 {
			r.Blocks[0].MessageGroup = nil
			r.Blocks[1].MessageGroup = nil
		}
		wire, _ := json.Marshal(r)
		command := exec.Command(bridge, "text")
		command.Stdin = bytes.NewReader(wire)
		ctext, err := command.Output()
		if err != nil || r.DisplayText() != string(ctext) {
			t.Fatalf("C/Go business projection differs: %q vs %q, %v", r.DisplayText(), ctext, err)
		}
	}

	// Both decoders reject corrupted enum/usage/contradiction at this boundary.
	for _, bad := range []string{strings.Replace(string(raw), `"stop_reason":5`, `"stop_reason":99`, 1), strings.Replace(string(raw), `"input":"9223372036854775807"`, `"input":"9223372036854775808"`, 1), strings.Replace(string(raw), `"stop_reason":5`, `"stop_reason":1`, 1), strings.Replace(string(raw), `"message_group":2`, `"message_group":4097`, 1), strings.Replace(string(raw), `"model":"model"`, `"model":null`, 1), strings.Replace(string(raw), `"stop_reason":5`, `"stop_reason":null`, 1), strings.Replace(string(raw), `"refusal":{"category":"future-category","explanation":null}`, `"refusal":{}`, 1)} {
		if _, err := DecodeResponse([]byte(bad)); err == nil {
			t.Fatal("Go admitted invalid C response")
		}
		command := exec.Command(bridge, "parse")
		command.Stdin = strings.NewReader(bad)
		if err := command.Run(); err == nil {
			t.Fatal("C unexpectedly admitted negative fixture")
		}
	}
}
func TestResponseDomainProjectionRequiresSuccessfulCanonicalTerminal(t *testing.T) {
	r := TextResponse(`{"verdict":"approve"}`)
	for _, stop := range []StopReason{StopUnknown, StopMaxTokens, StopToolUse, StopContentFilter, StopError} {
		r.StopReason = stop
		if _, err := r.SemanticText(); err == nil {
			t.Fatalf("stop %d became app authority", stop)
		}
	}
	r.StopReason = StopEndTurn
	if _, err := r.SemanticText(); err != nil {
		t.Fatal(err)
	}
	r.Blocks[0].Text = new(string)
	*r.Blocks[0].Text = string([]byte{255})
	if r.Valid() {
		t.Fatal("invalid UTF-8 struct admitted")
	}
}

func TestRequestCCodecConformance(t *testing.T) {
	bridge := cResponseBridge(t)
	fromC, err := exec.Command(bridge, "request-fixture").Output()
	if err != nil {
		t.Fatal(err)
	}
	request, err := DecodeRequest(fromC)
	if err != nil {
		t.Fatalf("Go rejects C canonical request: %v\n%s", err, fromC)
	}
	if len(request.Messages) != 3 || len(request.Tools) != 3 || request.Tools[1].Children[0].Name != "child" || !strings.Contains(string(request.Metadata), "9007199254740993") {
		t.Fatal("request lost modeled fields")
	}
	toC, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(bridge, "request")
	command.Stdin = bytes.NewReader(toC)
	roundtrip, err := command.Output()
	if err != nil {
		t.Fatalf("C rejects Go canonical request: %v\n%s", err, toC)
	}
	if !reflect.DeepEqual(jsonValue(t, fromC), jsonValue(t, roundtrip)) {
		t.Fatalf("C→Go→C request changed typed/opaque data:\n%s\n%s", fromC, roundtrip)
	}
	for _, bad := range []string{strings.Replace(string(fromC), `"version":1`, `"version":2`, 1), strings.Replace(string(fromC), `"max_tokens":2048`, `"max_tokens":2147483648`, 1), strings.Replace(string(fromC), `"system"`, `"raw":{},"system"`, 1), strings.Replace(string(fromC), `"text":"system"`, `"text":"system","tool_id":"hidden"`, 1)} {
		if _, err := DecodeRequest([]byte(bad)); err == nil {
			t.Fatal("Go admitted invalid request")
		}
		command := exec.Command(bridge, "request")
		command.Stdin = strings.NewReader(bad)
		if err := command.Run(); err == nil {
			t.Fatal("C admitted invalid request")
		}
	}
}
