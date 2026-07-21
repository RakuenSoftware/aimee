package engine

import (
	"context"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type recordingAgents struct {
	mu             sync.Mutex
	requests       []DelegateRequest
	reviewResponse string
}

func (a *recordingAgents) Delegate(_ context.Context, request DelegateRequest) (DelegateResult, error) {
	a.mu.Lock()
	a.requests = append(a.requests, request)
	a.mu.Unlock()
	if request.Role == "review" {
		response := a.reviewResponse
		if response == "" {
			response = `{"original_request_alignment":{"status":"aligned","summary":"implements the request"},"verdict":"approve","findings":[]}`
		}
		return DelegateResult{Response: response}, nil
	}
	return DelegateResult{Response: strings.Repeat("complete plan λ\n", 200_000) + "PLAN_END"}, nil
}

func TestNativeRoundtableFailsClosedOnOriginalRequestDriftOrOmission(t *testing.T) {
	tests := []struct {
		name, response, wantStatus string
		wantFindings               int
	}{
		{"drifted-with-finding", `{"original_request_alignment":{"status":"drifted","summary":"builds an unrelated dashboard"},"verdict":"changes","findings":[{"id":"bug","severity":"blocking","location":"x.go:1","summary":"concrete bug","recommendation":"fix it"}]}`, "drifted", 2},
		{"unclear", `{"original_request_alignment":{"status":"unclear","summary":"request context is insufficient"},"verdict":"approve","findings":[]}`, "unclear", 1},
		{"unknown", `{"original_request_alignment":{"status":"partial","summary":"only partly related"},"verdict":"approve","findings":[]}`, "unclear", 1},
		{"missing", `{"verdict":"approve","findings":[]}`, "unclear", 1},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			agents := &recordingAgents{reviewResponse: tc.response}
			runner := &NativeRunner{agents: agents}
			node := wfe.Node{Block: "gate.roundtable", Params: map[string]any{
				"quorum": 1, "max_rounds": 1,
				"panel": map[string]any{"required": []any{"original-request"}},
			}}
			reviewed := wfe.Artifact{Type: "plan", Content: []byte("unrelated direction")}
			reviewed.Hash = wfe.Hash(reviewed.Content)
			result, err := runner.roundtable(context.Background(), StepRequest{
				WorkItem: db1.WorkItem{Repo: "/repo", Worktree: "/worktree"},
				Node:     node, Proposal: "fix the scheduler", Inputs: map[string]wfe.Artifact{"src": reviewed},
			})
			if err != nil {
				t.Fatal(err)
			}
			if result.Status != StepChanges || result.Feedback == nil || len(result.Feedback.Findings) < tc.wantFindings {
				t.Fatalf("result=%+v", result)
			}
			if !strings.Contains(result.Feedback.Findings[0].Summary, "alignment is "+tc.wantStatus) {
				t.Fatalf("finding=%+v", result.Feedback.Findings[0])
			}
			if tc.name == "drifted-with-finding" {
				seenAlignment, seenBug := false, false
				for _, finding := range result.Feedback.Findings {
					seenAlignment = seenAlignment || strings.HasSuffix(finding.ID, "-original-request-alignment")
					seenBug = seenBug || finding.ID == "bug"
				}
				if !seenAlignment || !seenBug {
					t.Fatalf("alignment or concrete finding lost: %+v", result.Feedback.Findings)
				}
			}
		})
	}
}

func TestNativeRunnerUsesCompleteArtifactsAndOnlyPositiveUIPins(t *testing.T) {
	agents := &recordingAgents{}
	runner := &NativeRunner{agents: agents}
	proposal := strings.Repeat("proposal 漢字\n", 200_000) + "PROPOSAL_END"
	proposalArtifact := wfe.Artifact{Type: "proposal", Content: []byte(proposal), Hash: wfe.Hash([]byte(proposal))}
	planResult, err := runner.author(context.Background(), StepRequest{WorkItem: db1.WorkItem{Repo: "/repo"}, Node: wfe.Node{Params: map[string]any{}}, Proposal: proposal, Inputs: map[string]wfe.Artifact{"proposal": proposalArtifact}}, "plan")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(planResult.Artifact, "PLAN_END") {
		t.Fatal("plan response truncated")
	}
	node := wfe.Node{Block: "gate.roundtable", Params: map[string]any{"quorum": 2, "panel": map[string]any{
		"required": []any{"security", "qa"}, "eligible": []any{"contrarian"}, "pins": map[string]any{"security": "kimi"},
	}}}
	reviewed := wfe.Artifact{Type: "frozen_diff", Content: []byte(strings.Repeat("diff\n", 300_000) + "DIFF_END")}
	reviewed.Hash = wfe.Hash(reviewed.Content)
	result, err := runner.roundtable(context.Background(), StepRequest{WorkItem: db1.WorkItem{Repo: "/repo", Worktree: "/worktree"}, Node: node, Proposal: proposal, Inputs: map[string]wfe.Artifact{"src": reviewed}})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StepAdvanced {
		t.Fatalf("result=%+v", result)
	}
	agents.mu.Lock()
	defer agents.mu.Unlock()
	if len(agents.requests) != 4 {
		t.Fatalf("requests=%d", len(agents.requests))
	}
	foundPin, foundUnpinned := false, false
	for _, request := range agents.requests {
		if !strings.Contains(request.Prompt, "PROPOSAL_END") || request.Role == "review" && !strings.Contains(request.Prompt, "DIFF_END") {
			t.Fatal("runner truncated a source artifact")
		}
		if request.Role == "review" && (!strings.Contains(request.Prompt, "ORIGINAL REQUEST:") || strings.Contains(request.Prompt, "PROPOSAL:") || strings.Contains(request.Prompt, "complete proposal")) {
			t.Fatal("roundtable did not frame the source as the original request")
		}
		if request.Persona == "security" && request.Delegate == "kimi" {
			foundPin = true
		}
		if request.Persona == "qa" && request.Delegate == "" {
			foundUnpinned = true
		}
	}
	if !foundPin || !foundUnpinned {
		t.Fatalf("UI pin semantics not preserved: %+v", agents.requests)
	}
}

func TestExtractJSONObjectIgnoresProviderSuffix(t *testing.T) {
	expected := `{"schema_version":1,"summary":"brace } and escaped quote \" stay data","acceptance_criteria":["done"]}`
	response := "```json\n" +
		expected +
		"\n```\n$ git status\n" + `{"diagnostic":true}`
	doc, err := extractJSONObject(response)
	if err != nil {
		t.Fatal(err)
	}
	if got := string(doc); got != expected {
		t.Fatalf("wrong object extracted: %s", got)
	}
	if strings.Contains(string(doc), "diagnostic") {
		t.Fatalf("extracted trailing provider diagnostic: %s", doc)
	}
}

func TestExtractJSONObjectSkipsMalformedObjectPreamble(t *testing.T) {
	doc, err := extractJSONObject(`explanation {not json} then {"verdict":"approve","findings":[]}`)
	if err != nil {
		t.Fatal(err)
	}
	if got := string(doc); got != `{"verdict":"approve","findings":[]}` {
		t.Fatalf("wrong object extracted: %s", got)
	}
}

func TestExtractJSONObjectDoesNotPromoteNestedMalformedPayload(t *testing.T) {
	for _, response := range []string{
		`{"broken":,"payload":{"verdict":"approve","findings":[]}}`,
		`{"a":]}`,
		`{"broken":[} {"verdict":"approve","findings":[]} ]}`,
	} {
		if doc, err := extractJSONObject(response); err == nil {
			t.Fatalf("accepted nested or mismatched payload %q as %s", response, doc)
		}
	}
}

func TestExtractJSONObjectDoesNotPromoteObjectFromTopLevelArray(t *testing.T) {
	for _, response := range []string{
		`[{"ok":true}]`,
		`[broken,{"ok":true}]`,
		`[[{"ok":true}]]`,
		`[{"ok":true}`,
		`[{"a":1,`,
	} {
		if doc, err := extractJSONObject(response); err == nil {
			t.Fatalf("promoted nested array object from %q as %s", response, doc)
		}
	}
}

func TestExtractJSONObjectReturnsFirstAdjacentObject(t *testing.T) {
	doc, err := extractJSONObject(`{"a":1}{"b":2}`)
	if err != nil {
		t.Fatal(err)
	}
	if got := string(doc); got != `{"a":1}` {
		t.Fatalf("wrong object extracted: %s", got)
	}
}

func TestExtractJSONObjectAcceptsClosingBraceInsideString(t *testing.T) {
	expected := `{"a":"}"}`
	doc, err := extractJSONObject(expected)
	if err != nil {
		t.Fatal(err)
	}
	if got := string(doc); got != expected {
		t.Fatalf("wrong object extracted: %s", got)
	}
}

func TestExtractJSONObjectRejectsTruncatedAndProseResponses(t *testing.T) {
	for _, response := range []string{
		`{"a":"unterminated\\`,
		`{"a":"unterminated\`,
		`{"a":"\\\"}{"b":1}`,
		"provider returned prose",
		"{",
		`provider { broken {"a":1}`,
		`{"a":}}`,
		`{,}`,
		`{"a":1,}`,
	} {
		if doc, err := extractJSONObject(response); err == nil {
			t.Fatalf("accepted invalid response %q as %s", response, doc)
		}
	}
}

func TestExtractJSONObjectSkipsManyDisjointMalformedCandidates(t *testing.T) {
	const malformedCandidates = 10_000
	for _, malformed := range []string{`{,}`, `{"a":null,}`} {
		response := strings.Repeat(malformed, malformedCandidates) + `{"ok":true}`
		doc, err := extractJSONObject(response)
		if err != nil {
			t.Fatalf("failed after %d copies of %q: %v", malformedCandidates, malformed, err)
		}
		if got := string(doc); got != `{"ok":true}` {
			t.Fatalf("wrong object extracted after %d copies of %q: %s", malformedCandidates, malformed, got)
		}
	}
}

func TestExtractJSONObjectSkipsMalformedTokenCandidates(t *testing.T) {
	for _, malformed := range []string{`{"a":}}`, `{,}`, `{"a":1,}`} {
		doc, err := extractJSONObject(malformed + `{"ok":true}`)
		if err != nil {
			t.Fatalf("failed to recover after %q: %v", malformed, err)
		}
		if got := string(doc); got != `{"ok":true}` {
			t.Fatalf("wrong object extracted after %q: %s", malformed, got)
		}
	}
}

func TestExtractJSONObjectHandlesEscapedQuotesWithoutStateLeak(t *testing.T) {
	for _, expected := range []string{
		`{"a":"x\\\"y"}`,
		`{"a":"\\\""}`,
	} {
		response := expected + `{"ok":true}`
		doc, err := extractJSONObject(response)
		if err != nil {
			t.Fatalf("failed escaped-string input %q: %v", response, err)
		}
		if got := string(doc); got != expected {
			t.Fatalf("wrong escaped-string candidate from %q: %s", response, got)
		}
	}

	if doc, err := extractJSONObject("{\"a\":\"\\"); err == nil {
		t.Fatalf("accepted odd-backslash unterminated string as %s", doc)
	}
}

func TestExtractJSONObjectFailsClosedAfterMismatchedCandidate(t *testing.T) {
	for _, response := range []string{
		`{"a":[}{"ok":true}`,
		`[} {"ok":true}`,
		`["x",{"a":[} {"ok":true}]`,
	} {
		if doc, err := extractJSONObject(response); err == nil {
			t.Fatalf("accepted object after ambiguous framing %q as %s", response, doc)
		}
	}
}
