package engine

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type noCapacityGroupAgent struct{}

func (noCapacityGroupAgent) Delegate(context.Context, DelegateRequest) (DelegateResult, error) {
	return DelegateResult{}, errors.New("unexpected single dispatch")
}

func (noCapacityGroupAgent) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	out := make([]DelegateGroupResult, len(requests))
	for i := range out {
		out[i].Err = ErrNoFreeDelegateCapacity
	}
	return out
}

type admissionDeadlineGroupAgent struct{}

func (admissionDeadlineGroupAgent) Delegate(context.Context, DelegateRequest) (DelegateResult, error) {
	return DelegateResult{}, errors.New("unexpected single dispatch")
}

func (admissionDeadlineGroupAgent) DelegateGroup(_ context.Context, requests []DelegateRequest) []DelegateGroupResult {
	out := make([]DelegateGroupResult, len(requests))
	for i := range out {
		out[i].Err = ErrDelegateAdmissionWaitExpired
	}
	return out
}

func TestRoundtablePreservesNoFreeCapacityPause(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "default.json"), []byte(`{"name":"default","seats":[{"model":"$random","persona":"security"}],"min_successful":1}`), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := roundtablecfg.NewStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{agents: noCapacityGroupAgent{}, roundtables: store}
	artifact := wfe.Artifact{Type: "plan", Content: []byte("plan")}
	artifact.Hash = wfe.Hash(artifact.Content)
	result, err := runner.roundtable(context.Background(), StepRequest{
		WorkItem: db1.WorkItem{ID: "wi", Worktree: "/tmp"},
		Node: wfe.Node{ID: "gate", Block: "gate.roundtable", Params: map[string]any{
			"roundtable": "default",
		}},
		Proposal: "request", Inputs: map[string]wfe.Artifact{"src": artifact},
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StepPending || result.PauseReason != "panel_no_free_capacity" {
		t.Fatalf("result = %+v", result)
	}
	if result.Detail == "" {
		t.Fatal("capacity pause omitted seat outcome")
	}
}

func TestRoundtablePreservesAdmissionWaitDeadlinePause(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "default.json"), []byte(`{"name":"default","seats":[{"model":"$random","persona":"security"}],"min_successful":1}`), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := roundtablecfg.NewStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	runner := &NativeRunner{agents: admissionDeadlineGroupAgent{}, roundtables: store}
	artifact := wfe.Artifact{Type: "plan", Content: []byte("plan")}
	artifact.Hash = wfe.Hash(artifact.Content)
	result, err := runner.roundtable(context.Background(), StepRequest{
		WorkItem: db1.WorkItem{ID: "wi", Worktree: "/tmp"},
		Node: wfe.Node{ID: "gate", Block: "gate.roundtable", Params: map[string]any{
			"roundtable": "default",
		}},
		Proposal: "request", Inputs: map[string]wfe.Artifact{"src": artifact},
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != StepPending || result.PauseReason != "panel_admission_deadline" {
		t.Fatalf("result = %+v", result)
	}
	if !strings.Contains(result.Detail, "deadline_expired_while_waiting") {
		t.Fatalf("admission deadline detail = %q", result.Detail)
	}
}
