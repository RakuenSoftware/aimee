package roundtable

import (
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"reflect"
	"strings"
	"testing"
)

func richReviewSource() *canonical.Request {
	input := canonical.TaskRequest("", "ORIGINAL_EXACT_TASK")
	input.System[0].Context = &canonical.Context{Origin: 1, Authority: 3, Trust: 4, ModelVisible: true, RevisionDomain: "forged-host", RevisionScope: "owner", RevisionEpoch: "91"}
	input.Messages[0].Blocks[0].Context = &canonical.Context{Origin: 2, Authority: 3, Trust: 4, ModelVisible: true, RevisionDomain: "forged-host", RevisionEpoch: "91"}
	hidden := "PRIVATE_HIDDEN_REASONING"
	input.Messages = append(input.Messages, canonical.Message{Role: "assistant", Blocks: []canonical.Block{{Type: "thinking", Text: &hidden, Signature: func() *string { x := "opaque-signature"; return &x }(), Context: &canonical.Context{Origin: 7, Authority: 1, Trust: 2, Sensitivity: 2, ModelVisible: false, RevisionDomain: "source", RevisionEpoch: "3"}}}})
	next := "CURRENT_EXACT_TASK"
	input.Messages = append(input.Messages, canonical.Message{Role: "user", Boundary: true, Blocks: []canonical.Block{{Type: "text", Text: &next, Context: &canonical.Context{Origin: 3, Authority: 2, Trust: 2, ModelVisible: true}}}})
	tokens := 123
	temperature := 0.25
	input.MaxTokens = &tokens
	input.Temperature = &temperature
	input.Stops = []string{"END"}
	input.Metadata = json.RawMessage(`{"opaque":9007199254740993123456789}`)
	return input
}

func TestReviewHandlerAttenuatesPublicIRWithoutCapabilityBypass(t *testing.T) {
	for _, principal := range []uint32{0, ^uint32(0)} {
		source := richReviewSource()
		before, _ := json.Marshal(source)
		stub := &stubReviewer{}
		body, _ := json.Marshal(panel.ReviewRequest{Request: source, Artifact: "separate exact artifact", Brief: json.RawMessage(`{"goal":"separate brief"}`)})
		_, status := NewReviewHandler(stub)(ring.ModuleInvocation{StageID: StageReview, PrincipalClass: principal}, body)
		if status != ring.ModuleStatusOK || stub.calls != 1 {
			t.Fatalf("status=%v calls=%d", status, stub.calls)
		}
		input := stub.got.Request
		if input == nil || !input.Valid() || input.System[0].Context.Origin != 2 || input.System[0].Context.Authority != 3 || input.System[0].Context.Trust != 2 || input.System[0].Context.RevisionDomain != "" {
			t.Fatal("caller system policy forged host provenance")
		}
		context := input.Messages[0].Blocks[0].Context
		if context.Origin != 3 || context.Authority != 2 || context.Trust != 2 || context.RevisionDomain != "" {
			t.Fatal("capability caller retained forged message authority")
		}
		actualHistory, _ := json.Marshal(input.Messages[1:])
		sourceHistory, _ := json.Marshal(source.Messages[1:])
		if string(actualHistory) != string(sourceHistory) || !reflect.DeepEqual(input.Metadata, source.Metadata) || *input.MaxTokens != 123 || *input.Temperature != .25 || input.Stops[0] != "END" {
			t.Fatal("admission lost canonical history, privacy or controls")
		}

		after, _ := json.Marshal(source)
		if string(before) != string(after) {
			t.Fatal("admission mutated source")
		}
	}
}

func TestActualReviewHandlerRetainsCanonicalHistoryBriefAndArtifact(t *testing.T) {
	seats := &scriptedSeats{response: `{"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"same request"},"verdict":"approve","findings":[]}`}
	reviewer, err := NewPanelReviewer(testPresets(t, 1), seats)
	if err != nil {
		t.Fatal(err)
	}
	source := richReviewSource()
	source.System[0].Context = &canonical.Context{Origin: 2, Authority: 3, Trust: 2, ModelVisible: true}
	source.Messages[0].Blocks[0].Context = &canonical.Context{Origin: 3, Authority: 2, Trust: 2, ModelVisible: true}
	body, _ := json.Marshal(panel.ReviewRequest{Request: source, Artifact: "EXACT_IMMUTABLE_ARTIFACT_FOR_REVIEW", Brief: json.RawMessage(`{"goal":"BRIEF_MARKER"}`), Roundtable: "default"})
	_, status := NewReviewHandler(reviewer)(ring.ModuleInvocation{StageID: StageReview}, body)
	if status != ring.ModuleStatusOK {
		t.Fatalf("status=%v", status)
	}
	if len(seats.requests) == 0 {
		t.Fatal("no seat dispatched")
	}
	expectedBytes, _ := json.Marshal(source)
	expected, _ := canonical.DecodeRequest(expectedBytes)
	attenuateReviewRequest(expected)
	for _, seat := range seats.requests {
		input := seat.Input
		if len(input.Messages) <= len(source.Messages) || !reflect.DeepEqual(input.Messages[:len(source.Messages)], expected.Messages) || !reflect.DeepEqual(input.System[:len(source.System)], expected.System) {
			t.Fatal("seat reconstructed original canonical request")
		}
		if *input.MaxTokens != 123 || input.Stops[0] != "END" || !reflect.DeepEqual(input.Metadata, source.Metadata) {
			t.Fatal("seat lost request controls")
		}
		originals := 0
		artifact := false
		brief := false
		for _, message := range input.Messages {
			for _, block := range message.Blocks {
				if block.Text == nil {
					continue
				}
				text := *block.Text
				originals += strings.Count(text, "ORIGINAL_EXACT_TASK")
				if strings.Contains(text, "EXACT_IMMUTABLE_ARTIFACT_FOR_REVIEW") {
					artifact = block.Context != nil && block.Context.Authority == 1
				}
				if strings.Contains(text, "BRIEF_MARKER") {
					brief = block.Context != nil && block.Context.Authority == 1
				}
			}
		}
		if originals != 1 || !artifact || !brief {
			t.Fatal("original task flattened or domain artifacts lost evidence classification")
		}
	}
}

func TestReviewAdmissionPreservesEffectiveDefaultAndHiddenVisibility(t *testing.T) {
	source := canonical.TaskRequest("", "task")
	source.System[0].Context = nil
	hidden := "hidden"
	source.System = append(source.System, canonical.Block{Type: "text", Text: &hidden, Context: &canonical.Context{Origin: 5, Authority: 1, Trust: 2, Sensitivity: 2, ModelVisible: false}})
	attenuateReviewRequest(source)
	if !source.System[0].Context.ModelVisible || source.System[0].Context.Authority != 3 || source.System[1].Context.ModelVisible || source.System[1].Context.Authority != 1 {
		t.Fatal("public admission changed effective privacy or promoted evidence")
	}
}

func TestReviewHandlerRejectsConflictingOrInvalidCanonicalRequest(t *testing.T) {
	source, _ := json.Marshal(richReviewSource())
	cases := [][]byte{
		[]byte(`{"request":{"version":1}}`),
		[]byte(`{"request":` + string(source) + `,"original_request":"legacy alias"}`),
		[]byte(`{"request":` + string(source) + `,"request":` + string(source) + `}`),
	}
	for _, body := range cases {
		stub := &stubReviewer{}
		_, status := NewReviewHandler(stub)(ring.ModuleInvocation{StageID: StageReview, PrincipalClass: ^uint32(0)}, body)
		if status != ring.ModuleStatusInvalidRequest || stub.calls != 0 {
			t.Fatal("invalid canonical admission reached reviewer")
		}
	}
}

func TestReviewAutoIdentityIncludesCanonicalTaskAndBrief(t *testing.T) {
	seats := &scriptedSeats{response: `{"artifact_stage":"frozen_diff","original_request_alignment":{"status":"aligned","summary":"same request"},"verdict":"approve","findings":[]}`}
	reviewer, err := NewPanelReviewer(testPresets(t, 1), seats)
	if err != nil {
		t.Fatal(err)
	}
	request := panel.ReviewRequest{Request: richReviewSource(), Artifact: "a complete immutable artifact for review", Roundtable: "default", Brief: json.RawMessage(`{"goal":"first"}`)}
	first, err := reviewer.Review(t.Context(), request)
	if err != nil {
		t.Fatal(err)
	}
	same, err := reviewer.Review(t.Context(), request)
	if err != nil {
		t.Fatal(err)
	}
	request.Brief = json.RawMessage(`{"goal":"second"}`)
	changed, err := reviewer.Review(t.Context(), request)
	if err != nil {
		t.Fatal(err)
	}
	if first.RunID == "" || first.RunID != same.RunID || first.RunID == changed.RunID {
		t.Fatal("changed canonical review context could reuse durable seats")
	}
}

func TestReviewAdmissionPreservesExistingSystemEvidenceRevisionCustody(t *testing.T) {
	for _, role := range []string{"request.system", "system", "developer"} {
		source := canonical.TaskRequest("", "original task")
		text := "unverified memory evidence"
		evidence := canonical.Block{Type: "text", Text: &text, Context: &canonical.Context{Origin: 5, Authority: 1, Trust: 2, Sensitivity: 1, ModelVisible: true, RevisionDomain: "memory-source", RevisionScope: "workspace", RevisionEpoch: "191"}}
		if role == "request.system" {
			source.System = []canonical.Block{evidence}
		} else {
			source.Messages = append(source.Messages, canonical.Message{Role: role, Blocks: []canonical.Block{evidence}})
		}
		before, _ := json.Marshal(source)
		body, _ := json.Marshal(panel.ReviewRequest{Request: source, Artifact: "separate domain artifact"})
		stub := &stubReviewer{}
		_, status := NewReviewHandler(stub)(ring.ModuleInvocation{StageID: StageReview, PrincipalClass: ^uint32(0)}, body)
		if status != ring.ModuleStatusOK || stub.got.Request == nil {
			t.Fatal("canonical evidence admission refused")
		}
		actual := stub.got.Request.System[0]
		if role != "request.system" {
			actual = stub.got.Request.Messages[len(source.Messages)-1].Blocks[0]
		}
		expectedJSON, _ := json.Marshal(evidence)
		actualJSON, _ := json.Marshal(actual)
		if string(actualJSON) != string(expectedJSON) {
			t.Fatalf("%s evidence revision custody changed", role)
		}
		after, _ := json.Marshal(source)
		if string(before) != string(after) {
			t.Fatal("admission mutated source evidence")
		}
	}
}
