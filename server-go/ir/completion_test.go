package ir

import (
	"bytes"
	"encoding/json"
	"os/exec"
	"testing"
)

type completionCase struct {
	name           string
	response       Response
	complete, text bool
}

func completionCases() []completionCase {
	base := *TextResponse(`{"decision":"review"}`)
	image := ResponseBlock{Block: Block{Type: "image", MediaType: "image/png", MediaRef: "data:image/png;base64,AAAA"}, Raw: json.RawMessage(`{"opaque":[17,"kept"]}`)}
	document := ResponseBlock{Block: Block{Type: "document", MediaType: "application/pdf", MediaRef: "fixture.pdf"}}
	reasoning := ""
	encrypted := "encrypted-state"
	thinking := ResponseBlock{Block: Block{Type: "thinking", Text: &reasoning, EncryptedContent: &encrypted}}
	makeCase := func(name string, blocks []ResponseBlock, complete, text bool) completionCase {
		r := base
		r.Blocks = blocks
		return completionCase{name, r, complete, text}
	}
	cases := []completionCase{
		{"text", base, true, true},
		makeCase("image", []ResponseBlock{image}, true, false),
		makeCase("document", []ResponseBlock{document}, true, false),
		makeCase("text-image-opaque", []ResponseBlock{thinking, base.Blocks[0], image}, true, false),
		makeCase("image-thinking", []ResponseBlock{thinking, image}, true, false),
		makeCase("text-thinking", []ResponseBlock{thinking, base.Blocks[0]}, true, true),
		makeCase("thinking-only", []ResponseBlock{thinking}, false, false),
		makeCase("empty", []ResponseBlock{}, false, false),
		makeCase("pending-tool", []ResponseBlock{base.Blocks[0], {Block: Block{Type: "tool_use", ToolID: "pending", Name: "write", Input: json.RawMessage(`{}`)}}}, false, false),
	}
	for _, stop := range []StopReason{StopUnknown, StopMaxTokens, StopToolUse, StopContentFilter, StopError} {
		r := base
		r.StopReason = stop
		cases = append(cases, completionCase{name: "noncomplete-stop-" + string(rune('0'+stop)), response: r})
	}
	r := base
	r.StopReason = StopSequence
	cases = append(cases, completionCase{"sequence", r, true, true})
	r = base
	message := "diagnostic"
	r.ErrorMessage = &message
	cases = append(cases, completionCase{name: "error", response: r})
	r = base
	r.StopReason = StopContentFilter
	r.Refusal = &RefusalDetails{}
	cases = append(cases, completionCase{name: "refusal", response: r})
	whitespace := " \n"
	r = base
	r.Blocks = []ResponseBlock{{Block: Block{Type: "text", Text: &whitespace}}}
	cases = append(cases, completionCase{"whitespace-model-completion", r, true, true})
	return cases
}
func TestGeneralCompletionAndBusinessTextAreDistinct(t *testing.T) {
	for _, c := range completionCases() {
		t.Run(c.name, func(t *testing.T) {
			if c.response.Complete() != c.complete || c.response.CompleteText() != c.text {
				t.Fatal("completion predicate granted incorrect authority")
			}
			if !c.text {
				if _, err := c.response.SemanticText(); err == nil {
					t.Fatal("non-text completion acquired business authority")
				}
			}
		})
	}
}

func TestGeneralAndTextCompletionMatchProductionC(t *testing.T) {
	bridge := cResponseBridge(t)
	for _, c := range completionCases() {
		t.Run(c.name, func(t *testing.T) {
			wire, err := json.Marshal(c.response)
			if err != nil {
				t.Fatal(err)
			}
			command := exec.Command(bridge, "completion")
			command.Stdin = bytes.NewReader(wire)
			result, err := command.Output()
			if err != nil {
				t.Fatalf("C admission/predicate failed: %v", err)
			}
			var got struct {
				Complete bool `json:"complete"`
				Text     bool `json:"complete_text"`
			}
			if json.Unmarshal(result, &got) != nil || got.Complete != c.response.Complete() || got.Text != c.response.CompleteText() {
				t.Fatalf("C/Go completion disagreement: %s", result)
			}
		})
	}
}
