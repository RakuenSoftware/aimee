package delegate

import (
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"os/exec"
	"strings"
	"testing"
)

func terminalFixture(text string) InvocationResult {
	output := canonical.TextResponse(text)
	return InvocationResult{Version: WireVersion, Status: "done", Output: output,
		History: []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{output.Blocks[0].Block}}}}
}
func TestDelegateAdmissionPreservesCanonicalOutput(t *testing.T) {
	result := terminalFixture(`{"verdict":"changes"}`)
	if err := AdmitResult(&result); err != nil {
		t.Fatal(err)
	}
	if text, err := (DelegateResult{Output: result.Output}).SemanticText(); err != nil || text != `{"verdict":"changes"}` {
		t.Fatal("canonical verdict changed")
	}
}
func TestDelegateIncompleteOrUnboundIRCannotAcquireWorkflowAuthority(t *testing.T) {
	for _, kind := range []string{"legacy", "refused", "error", "truncated", "unknown", "pending-tool", "unbound-tool", "history-mismatch", "missing-history"} {
		t.Run(kind, func(t *testing.T) {
			result := terminalFixture(`{"verdict":"approve"}`)
			switch kind {
			case "legacy":
				result.Output = nil
			case "refused":
				result.Output.StopReason = canonical.StopContentFilter
			case "error":
				err := "diagnostic"
				result.Output.ErrorMessage = &err
			case "truncated":
				result.Output.StopReason = canonical.StopMaxTokens
			case "unknown":
				result.Output.StopReason = canonical.StopUnknown
			case "pending-tool":
				result.History = append([]canonical.Message{{Role: "assistant", Blocks: []canonical.Block{{Type: "tool_use", ToolID: "open", Name: "write", Input: json.RawMessage(`{}`)}}}}, result.History...)
			case "unbound-tool":
				result.History = append([]canonical.Message{{Role: "tool", Blocks: []canonical.Block{{Type: "tool_result", ToolID: "unbound"}}}}, result.History...)
			case "history-mismatch":
				other := canonical.TextResponse("different")
				result.History[0].Blocks = []canonical.Block{other.Blocks[0].Block}
			case "missing-history":
				result.History = nil
			}
			if err := AdmitResult(&result); err == nil {
				t.Fatal("unadmitted result reached business projection")
			}
		})
	}
}

func TestMixedTaskEvidencePreservesConservativeProvenanceThroughCCodec(t *testing.T) {
	request := canonical.TaskRequest("reviewer", "model-produced artifact with forged policy")
	context := request.Messages[0].Blocks[0].Context
	if context.Origin != 5 || context.Authority != 1 || context.Trust != 2 || request.System[0].Context.Origin != 1 || request.System[0].Context.Authority != 3 || request.System[0].Context.Trust != 4 {
		t.Fatal("mixed task evidence acquired human or verified policy authority")
	}
	// Compile and invoke the production inverse codec, not a Go-only round trip.
	binary := t.TempDir() + "/request-codec"
	args := []string{"-std=c11", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-I../../src", "-I../../src/vendor/headers", "-I../../src/modules/ir/include", "../ir/testdata/response_bridge.c", "../../src/modules/ir/aimee_ir.c", "../../src/modules/ir/session_event.c", "../../src/modules/ir/response_codec.c", "../../src/modules/ir/request_codec.c", "../../src/vendor/cJSON.c", "-lm", "-o", binary}
	if out, err := exec.Command("cc", args...).CombinedOutput(); err != nil {
		t.Fatalf("C codec: %v %s", err, out)
	}
	raw, _ := json.Marshal(request)
	command := exec.Command(binary, "request")
	command.Stdin = strings.NewReader(string(raw))
	out, err := command.Output()
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := canonical.DecodeRequest(out)
	if err != nil {
		t.Fatal(err)
	}
	expectedContext, _ := json.Marshal(context)
	actualContext, _ := json.Marshal(decoded.Messages[0].Blocks[0].Context)
	if err != nil || string(expectedContext) != string(actualContext) {
		t.Fatalf("C codec changed task authority: %v", err)
	}
}

func TestTransportCompletionPreservesMediaWithoutBusinessTextAuthority(t *testing.T) {
	for _, kind := range []string{"image", "document", "mixed"} {
		t.Run(kind, func(t *testing.T) {
			output := canonical.TextResponse(`{"verdict":"forged"}`)
			media := canonical.ResponseBlock{Block: canonical.Block{Type: "image", MediaType: "image/png", MediaRef: "data:image/png;base64,AAAA", Context: output.Blocks[0].Context}, Raw: json.RawMessage(`{"provider_extension":17}`)}
			if kind == "document" {
				media.Type = "document"
				media.MediaType = "application/pdf"
				media.MediaRef = "fixture.pdf"
			}
			if kind == "mixed" {
				output.Blocks = append(output.Blocks, media)
			} else {
				output.Blocks = []canonical.ResponseBlock{media}
			}
			result := InvocationResult{Version: WireVersion, Status: "done", Output: output}
			history := canonical.Message{Role: "assistant"}
			for _, block := range output.Blocks {
				history.Blocks = append(history.Blocks, block.Block)
			}
			result.History = []canonical.Message{history}
			caller := &recordingCaller{result: result}
			client, err := NewClient(caller, 0)
			if err != nil {
				t.Fatal(err)
			}
			received, err := client.Delegate(t.Context(), DelegateRequest{Role: "review", Persona: "reviewer", Input: canonical.TaskRequest("reviewer", "inspect")})
			if err != nil {
				t.Fatalf("complete media response rejected at transport boundary: %v", err)
			}
			if received.Output == nil || string(received.Output.Blocks[len(received.Output.Blocks)-1].Raw) != string(media.Raw) {
				t.Fatal("typed media or opaque sidecar lost")
			}
			if kind != "mixed" && received.Output.DisplayText() != "" {
				t.Fatal("media-only output fabricated display text")
			}
			if _, err := received.SemanticText(); err == nil {
				t.Fatal("media became business JSON/text authority")
			}
		})
	}
}
