package panel

import (
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"reflect"
	"strings"
	"testing"
)

func TestCanonicalReviewInputPreservesSourceAndSeparatesProvenance(t *testing.T) {
	source := canonical.TaskRequest("", "EXACT_ORIGINAL_TASK")
	hidden := "signed hidden source"
	signature := "opaque"
	source.Messages = append(source.Messages, canonical.Message{Role: "assistant", Blocks: []canonical.Block{{Type: "thinking", Text: &hidden, Signature: &signature, Context: &canonical.Context{Origin: 7, Authority: 1, Trust: 2, ModelVisible: false, RevisionDomain: "source", RevisionEpoch: "31"}}}})
	max := 981
	temp := .4
	source.MaxTokens = &max
	source.Temperature = &temp
	source.Stops = []string{"STOP"}
	source.Metadata = json.RawMessage(`{"opaque":9007199254740993123456789}`)
	before, _ := json.Marshal(source)
	run := Run{Request: source, Focus: "USER_FOCUS", Brief: json.RawMessage(`{"goal":"USER_BRIEF"}`), Reviewed: Artifact{Stage: "plan", Content: "ARTIFACT_BYTES"}}
	input, err := reviewInput(run, "security", "HOST_REVIEW_PROCEDURE", "MODEL_REPORT_BYTES")
	if err != nil {
		t.Fatal(err)
	}
	prefix, _ := json.Marshal(input.Messages[:len(source.Messages)])
	original, _ := json.Marshal(source.Messages)
	if string(prefix) != string(original) || !reflect.DeepEqual(input.Metadata, source.Metadata) || *input.MaxTokens != 981 || *input.Temperature != .4 || input.Stops[0] != "STOP" {
		t.Fatal("review rebuilt canonical source/control fields")
	}
	blocks := input.Messages[len(source.Messages)].Blocks
	for _, b := range blocks {
		if b.Text == nil {
			continue
		}
		switch {
		case *b.Text == "HOST_REVIEW_PROCEDURE":
			if b.Context.Authority != 2 || b.Context.Origin != 1 {
				t.Fatal("host procedure not typed task")
			}
		case strings.Contains(*b.Text, "MODEL_REPORT_BYTES"):
			if b.Context.Authority != 1 || b.Context.Origin != 7 {
				t.Fatal("model report promoted to instruction")
			}
		case strings.Contains(*b.Text, "ARTIFACT_BYTES"), strings.Contains(*b.Text, "USER_BRIEF"):
			if b.Context.Authority != 1 {
				t.Fatal("domain artifact promoted to instruction")
			}
		case strings.Contains(*b.Text, "USER_FOCUS"):
			if b.Context.Origin != 3 || b.Context.Authority != 2 {
				t.Fatal("user focus promoted to host policy")
			}
		}
	}
	changed := "changed"
	input.Messages[0].Blocks[0].Text = &changed
	after, _ := json.Marshal(source)
	if string(before) != string(after) {
		t.Fatal("review clone aliases source")
	}
}

func TestDiscussionPartsKeepParticipantDataOutOfHostProcedure(t *testing.T) {
	reports := []discussionTranscriptReport{{Persona: "UNTRUSTED_REPORT_MARKER"}}
	procedure, data := buildDiscussionParts("run", "hash", 1, reports, nil)
	if strings.Contains(procedure, "UNTRUSTED_REPORT_MARKER") || !strings.Contains(data, "UNTRUSTED_REPORT_MARKER") {
		t.Fatal("participant data mixed into host procedure")
	}
	input, err := reviewInput(Run{Request: canonical.TaskRequest("", "original")}, "reviewer", procedure, data)
	if err != nil {
		t.Fatal(err)
	}
	for _, b := range input.Messages[len(input.Messages)-1].Blocks {
		if b.Text != nil && strings.Contains(*b.Text, "UNTRUSTED_REPORT_MARKER") && b.Context.Authority != 1 {
			t.Fatal("participant report promoted")
		}
	}
}
