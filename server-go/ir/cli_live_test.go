package ir

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"testing"
	"time"
)

// The primary integration supplies the genuine CLI and counts four requests at
// its local provider. This is a generic canonical consumer, not a memory owner
// or benchmark policy: incomplete/refused/tool output cannot become answer text.
func TestCanonicalResponseLiveRegisteredCLI(t *testing.T) {
	executable := os.Getenv("AIMEE_QA_LIVE_CLI")
	if executable == "" {
		t.Skip("requires the canonical primary local-provider integration")
	}
	for _, test := range []struct {
		prompt string
		stop   StopReason
	}{
		{"IR_QA_ANSWER", StopEndTurn},
		{"IR_QA_PARTIAL", StopMaxTokens},
		{"IR_QA_REFUSED", StopContentFilter},
		{"IR_QA_TOOL", StopToolUse},
	} {
		t.Run(test.prompt, func(t *testing.T) {
			block := func(text string, origin, authority int) Block {
				return Block{Type: "text", Text: &text, Context: &Context{Origin: origin,
					Authority: authority, Trust: 2, Sensitivity: 1, ModelVisible: true, RevisionEpoch: "0"}}
			}
			tokens, temperature := 256, .3
			request := Request{Version: 1, SourceWire: "unspecified", CachePolicy: "native",
				System:   []Block{block("Use only the supplied evidence.", 3, 2)},
				Messages: []Message{{Role: "user", ContentScalar: true, Blocks: []Block{block(test.prompt, 5, 1)}}},
				Tools:    []Tool{}, Stops: []string{}, ToolChoice: ToolChoice{Mode: "none"},
				MaxTokens: &tokens, Temperature: &temperature}
			if !request.Valid() {
				t.Fatal("invalid canonical fixture request")
			}
			wire, err := json.Marshal(map[string]any{"ir": request})
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(t.Context(), 30*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, executable, "--json", "model", "generate")
			command.Stdin = bytes.NewReader(wire)
			output, runErr := command.Output()
			var envelope map[string]json.RawMessage
			if !Decode(output, &envelope) {
				t.Fatalf("CLI canonical envelope: %s (%v)", output, runErr)
			}
			response, err := DecodeResponse(envelope["ir"])
			if err != nil || response.StopReason != test.stop || response.Usage.Input != "10" || response.Usage.Output != "2" {
				t.Fatalf("CLI canonical observation: %s (%v)", output, err)
			}
			text, textErr := response.SemanticText()
			if test.stop == StopEndTurn {
				if runErr != nil || textErr != nil || text != "PRIMARY_OK" {
					t.Fatal(text, runErr, textErr)
				}
			} else if runErr == nil || textErr == nil || text != "" {
				t.Fatal("incomplete/refused/tool output promoted to answer", text, runErr, textErr)
			}
		})
	}
}
