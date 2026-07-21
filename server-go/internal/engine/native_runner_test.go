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
	mu       sync.Mutex
	requests []DelegateRequest
}

func (a *recordingAgents) Delegate(_ context.Context, request DelegateRequest) (DelegateResult, error) {
	a.mu.Lock()
	a.requests = append(a.requests, request)
	a.mu.Unlock()
	if request.Role == "review" {
		return DelegateResult{Response: `{"verdict":"approve","findings":[]}`}, nil
	}
	return DelegateResult{Response: strings.Repeat("complete plan λ\n", 200_000) + "PLAN_END"}, nil
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
