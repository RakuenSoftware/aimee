package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestHealthExecutionReceiptUsesOwnedExactScope(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	binding := providerReceiptBinding{TurnID: "ingress-turn", Project: "app", Workspace: "work"}
	digest := strings.Repeat("a", 64)
	entry := &sourceReleaseEntry{assemblyMetadata: json.RawMessage(`{"projection_commitment":"unchanged"}`), assemblyParts: []sourceReleasePart{{CoverageStatus: "complete", RequirementDigest: strings.Repeat("b", 64)}}}
	fields := map[string]string{"principal": "alice", "session": "owned-session", "task": "session-task", "project": "app", "workspace": "work"}
	args := commandArgs{"principal": json.RawMessage(`"alice"`)}
	set := func() {
		wire, _ := json.Marshal(fields)
		args["health_execution_binding"], _ = json.Marshal(string(wire))
	}
	set()
	read := func(raw json.RawMessage) *healthExecutionContext {
		var output struct {
			Execution *healthExecutionContext `json:"health_execution"`
		}
		if err := json.Unmarshal(raw, &output); err != nil {
			t.Fatal(err)
		}
		return output.Execution
	}
	context := read(receiptMetadataWithExecution(entry, args, binding, digest))
	if context == nil || !context.valid(digest) || context.QueryClass != "typed_requirements" {
		t.Fatal(context)
	}
	first := *context
	for _, key := range []string{"principal", "project", "workspace"} {
		old := fields[key]
		fields[key] = "foreign"
		set()
		if read(receiptMetadataWithExecution(entry, args, binding, digest)) != nil {
			t.Fatalf("accepted mismatched %s", key)
		}
		fields[key] = old
	}
	fields["session"] = "another-session"
	set()
	if context = read(receiptMetadataWithExecution(entry, args, binding, digest)); context == nil || context.Task == first.Task || context.Turn == first.Turn {
		t.Fatal("session task aliases shared an identity")
	}
	fields["session"] = "owned-session"
	set()
	binding.TurnID = "next-ingress-turn"
	if context = read(receiptMetadataWithExecution(entry, args, binding, digest)); context == nil || context.Task != first.Task || context.Turn == first.Turn {
		t.Fatal("task/turn identity is unstable")
	}
	if context.valid(strings.Repeat("c", 64)) {
		t.Fatal("metadata transferred across receipt bindings")
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "0")
	if read(receiptMetadataWithExecution(entry, args, binding, digest)) != nil {
		t.Fatal("rollback retained optional capture")
	}
}

func TestHealthExecutionImporterDoesNotInventPreviousTurn(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	state := &sourceReleaseState{}
	plan := sourceReleaseCall(t, state, receiptTestAdmission(t, state))
	var prepared providerReceiptEvent
	if err := json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared); err != nil {
		t.Fatal(err)
	}
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	execution := healthExecutionContext{Binding: prepared.BindingDigest, Task: strings.Repeat("a", 64), Turn: strings.Repeat("b", 64), QueryClass: "typed_requirements", Source: "host_exploration_binding"}
	raw, _ := json.Marshal(map[string]any{"health_execution": execution})
	r := inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: raw}
	snapshot, err := healthSnapshotFromReceipt(r, j, 1000000)
	if err != nil || snapshot.Invocation.Task != execution.Task || snapshot.Invocation.Turn != execution.Turn || snapshot.Invocation.QueryClass != "typed_requirements" || snapshot.Invocation.PreviousTurn != "" {
		t.Fatal(snapshot, err)
	}
	if !strings.Contains(strings.Join(snapshot.Invocation.MetadataGaps, ","), "previous_eligible_turn") {
		t.Fatal("adjacency gap hidden")
	}
	execution.Binding = strings.Repeat("c", 64)
	r.Assembly, _ = json.Marshal(map[string]any{"health_execution": execution})
	snapshot, err = healthSnapshotFromReceipt(r, j, 1000000)
	if err != nil || snapshot.Invocation.Task != "" {
		t.Fatal("foreign receipt task imported", err)
	}
}
