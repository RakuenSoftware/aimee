package executionpolicy

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestSessionExplorationSharesOperatorAcrossTasks(t *testing.T) {
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if err := os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"exploration":{"raw_scans":2}}`), 0600); err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	var state []byte
	apply := func(req sessionExplorationRequest) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(req)
		next, reply, err := SessionExploration("alice", "session", state, raw, now)
		if err != nil {
			t.Fatal(err)
		}
		state = next
		var value map[string]any
		json.Unmarshal(reply, &value)
		return value
	}
	c := testContract(now)
	for _, task := range []string{"task-a", "task-b", "task-c"} {
		c.Binding.Task = task
		c.ID = task
		apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
		a := attempt(c, task)
		result := apply(sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a})
		if result["restricted"] != (task == "task-c") {
			t.Fatalf("%s: %v", task, result)
		}
	}
	// A reissued task contract does not reset the session ceiling.
	c.PlanDigest = "new-plan"
	apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
	a := attempt(c, "revision")
	if result := apply(sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a}); result["restricted"] != true {
		t.Fatal("revision reset session ceiling")
	}
	raw, _ := json.Marshal(sessionExplorationRequest{Operation: "inspect", Binding: c.Binding})
	if _, _, err := SessionExploration("mallory", "session", state, raw, now); err == nil {
		t.Fatal("cross-principal state access")
	}
	var decoded sessionExplorationState
	json.Unmarshal(state, &decoded)
	decoded.Tasks[c.Binding.Task] = explorationSnapshot{}
	corrupt, _ := json.Marshal(decoded)
	if _, _, err := SessionExploration("alice", "session", corrupt, raw, now); err == nil {
		t.Fatal("corrupt state accepted")
	}
}

func TestSessionRecoveryRequiresObservedIndexedGap(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Binding.Workspace = "local:project"
	c.Binding.WorkingDirectory = "/project"
	c.Limits.RawScans = ceiling(0)
	var state []byte
	apply := func(req sessionExplorationRequest) (map[string]any, error) {
		raw, _ := json.Marshal(req)
		next, reply, e := SessionExploration("alice", "session", state, raw, now)
		if e != nil {
			return nil, e
		}
		state = next
		var out map[string]any
		json.Unmarshal(reply, &out)
		now = now.Add(time.Second)
		return out, nil
	}
	if _, e := apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c}); e != nil {
		t.Fatal(e)
	}
	forged := sessionExplorationRequest{Operation: "observe_indexed", Binding: c.Binding, Tool: "bash", ToolResult: "[]", AttemptID: "invented"}
	if _, e := apply(forged); e == nil {
		t.Fatal("non-indexed tool granted a recovery gap")
	}
	observed := sessionExplorationRequest{Operation: "observe_indexed", Binding: c.Binding, Tool: "code_search", ToolResult: "[]", Arguments: json.RawMessage(`{"query":"missing-definition"}`), AttemptID: "lookup-1"}
	gap, e := apply(observed)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = apply(observed); e != nil {
		t.Fatal("observation retry failed", e)
	}
	expand := sessionExplorationRequest{Operation: "expand", Binding: c.Binding, Reason: "supplied evidence is insufficient", Gap: gap["gap_ref"].(string), OutcomeID: "lookup-1"}
	bad := expand
	bad.Gap = "model-invented"
	if _, e = apply(bad); e == nil {
		t.Fatal("invented gap accepted")
	}
	if _, e = apply(expand); e != nil {
		t.Fatal(e)
	}
	if _, e = apply(expand); e != nil {
		t.Fatal("expansion retry", e)
	}
	for i := 0; i < 2; i++ {
		a := attempt(c, fmt.Sprintf("raw-%d", i))
		if _, e = apply(sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a}); e != nil {
			t.Fatal(e)
		}
		observed.AttemptID = fmt.Sprintf("lookup-%d", i+2)
		if _, e = apply(observed); e != nil {
			t.Fatal(e)
		}
		turn := sessionExplorationRequest{Operation: "observe_turn", Binding: c.Binding, TurnID: fmt.Sprint(i)}
		if _, e = apply(turn); e != nil {
			t.Fatal(e)
		}
		if _, e = apply(turn); e != nil {
			t.Fatal(e)
		}
	}
	var saved sessionExplorationState
	json.Unmarshal(state, &saved)
	task := saved.Tasks[c.Binding.Task]
	if task.StarvedTurns != 2 || task.LastTurn != 2 || len(task.CompletedTurns) != 2 {
		t.Fatalf("turn replay or observed starvation lost: %+v", task)
	}
	if len(task.Revisions) != 2 || task.Revisions[0].ConfidenceProvenance != task.Revisions[1].ConfidenceProvenance {
		t.Fatal("recovery rewrote confidence or lost history")
	}
}

func TestSessionAdmissionIsNotRefundableAndIndexedFormatsAreExact(t *testing.T) {
	t.Setenv("AIMEE_HOME", t.TempDir())
	now := time.Now()
	c := testContract(now)
	c.Binding.Workspace = "local:project"
	c.Binding.WorkingDirectory = "/project"
	var state []byte
	apply := func(req sessionExplorationRequest) (map[string]any, error) {
		t.Helper()
		raw, _ := json.Marshal(req)
		next, reply, err := SessionExploration("alice", "session", state, raw, now)
		if err != nil {
			return nil, err
		}
		state = next
		var out map[string]any
		json.Unmarshal(reply, &out)
		return out, nil
	}
	if _, err := apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c}); err != nil {
		t.Fatal(err)
	}
	check := sessionExplorationRequest{Operation: "check", Binding: c.Binding, Tool: "bash", Arguments: json.RawMessage(`{"command":"rg definition"}`), AttemptID: "dispatch-1"}
	if out, err := apply(check); err != nil || out["allowed"] != true {
		t.Fatal(out, err)
	}
	if _, err := apply(sessionExplorationRequest{Operation: "cancel_before_dispatch", Binding: c.Binding, AttemptID: check.AttemptID}); err == nil {
		t.Fatal("possibly dispatched admission refunded")
	}
	if _, err := apply(check); err != nil {
		t.Fatal("admission retry", err)
	}
	var saved sessionExplorationState
	json.Unmarshal(state, &saved)
	if saved.Usage.RawScans != 1 {
		t.Fatal("retry double charged")
	}
	observation := sessionExplorationRequest{Operation: "observe_indexed", Binding: c.Binding, Tool: "find_symbol", Arguments: json.RawMessage(`{"identifier":"missing"}`), AttemptID: "symbol-1", ToolResult: "No symbol found for 'missing'"}
	if out, err := apply(observation); err != nil || out["expansion_available"] != true {
		t.Fatal("native symbol miss lost", out, err)
	}
	observation.AttemptID = "symbol-2"
	observation.ToolResult += "\nactual result"
	if out, err := apply(observation); err != nil || out["expansion_available"] != nil {
		t.Fatal("partial hit granted empty fallback", out, err)
	}
	observation.Tool = "code_search"
	observation.ToolResult = "[]"
	observation.Arguments = json.RawMessage(`{"query":"missing","project":"other-project"}`)
	if _, err := apply(observation); err == nil {
		t.Fatal("cross-project lookup granted fallback")
	}
}

func TestExternalToolBindingUsesOwnedSessionAndExecutionDirectory(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.ID, c.Binding.Task = "session-task", "session-task"
	c.Binding.WorkingDirectory = t.TempDir()
	raw, _ := json.Marshal(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
	state, _, err := SessionExploration("alice", "session", nil, raw, now)
	if err != nil {
		t.Fatal(err)
	}
	req := sessionExplorationRequest{Operation: "bind_session", Path: c.Binding.WorkingDirectory}
	// A client-created binding is never used for this host owner lookup.
	req.Binding.Principal = "mallory"
	raw, _ = json.Marshal(req)
	_, reply, err := SessionExploration("alice", "session", state, raw, now)
	var out struct {
		Binding explorationBinding `json:"binding"`
	}
	if err != nil || json.Unmarshal(reply, &out) != nil || out.Binding != c.Binding {
		t.Fatal("owned binding unavailable", err)
	}
	if _, _, err = SessionExploration("mallory", "session", state, raw, now); err == nil {
		t.Fatal("foreign principal obtained binding")
	}
	req.Path = t.TempDir()
	raw, _ = json.Marshal(req)
	if _, _, err = SessionExploration("alice", "session", state, raw, now); err == nil {
		t.Fatal("different worktree rebound contract")
	}
}

func TestSessionExplorationChildrenShareTaskAllowance(t *testing.T) {
	t.Setenv("AIMEE_HOME", t.TempDir())
	now := time.Now()
	var state []byte
	apply := func(req sessionExplorationRequest) map[string]any {
		t.Helper()
		raw, _ := json.Marshal(req)
		next, reply, err := SessionExploration("alice", "session", state, raw, now)
		if err != nil {
			t.Fatal(err)
		}
		state = next
		var result map[string]any
		json.Unmarshal(reply, &result)
		return result
	}
	c := testContract(now)
	c.Limits.Enabled = true
	c.Limits.RawScans = ceiling(1)
	for i, task := range []string{"parent", "child-a", "child-b"} {
		c.ID = task
		c.Binding.Task = task
		c.Binding.BudgetTask = "parent"
		apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
		a := attempt(c, task)
		req := sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a}
		result := apply(req)
		if result["would_restrict"] != (i > 0) || result["restricted"] != false {
			t.Fatalf("%s: %v", task, result)
		}
		apply(req) // Same admitted child attempt never spends twice.
	}
	var decoded sessionExplorationState
	json.Unmarshal(state, &decoded)
	if decoded.Groups["parent"].RawScans != 3 || decoded.Usage.RawScans != 3 {
		t.Fatal(decoded)
	}
	// Task-specific revisions cannot move their spent allowance to a new root.
	c.Binding.BudgetTask = "fresh-group"
	raw, _ := json.Marshal(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
	if _, _, err := SessionExploration("alice", "session", state, raw, now); err == nil {
		t.Fatal("revision transferred its budget group")
	}
	// An unrelated task keeps its own allowance, while session work stays global.
	c.ID = "unrelated"
	c.Binding.Task = "unrelated"
	c.Binding.BudgetTask = ""
	apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
	a := attempt(c, "unrelated")
	if apply(sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a})["would_restrict"] != false {
		t.Fatal("unrelated task inherited adaptive debt")
	}
}

func TestSessionExplorationLegacyTaskUsageSurvivesGroupUpgrade(t *testing.T) {
	t.Setenv("AIMEE_HOME", t.TempDir())
	now := time.Now()
	c := testContract(now)
	c.Limits.Enabled = true
	c.Limits.RawScans = ceiling(2)
	old := sessionExplorationState{Version: 1, Principal: "alice", Session: "session", Usage: explorationUsage{RawScans: 2}, Tasks: map[string]explorationSnapshot{
		c.Binding.Task: {Revisions: []explorationContract{c}, TaskUsage: explorationUsage{RawScans: 2}},
	}}
	state, _ := json.Marshal(old)
	a := attempt(c, "after-upgrade")
	raw, _ := json.Marshal(sessionExplorationRequest{Operation: "reserve", Binding: c.Binding, Attempt: &a})
	next, reply, err := SessionExploration("alice", "session", state, raw, now)
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]any
	json.Unmarshal(reply, &result)
	if result["would_restrict"] != true {
		t.Fatal("upgrade reset legacy task usage", string(reply))
	}
	var decoded sessionExplorationState
	json.Unmarshal(next, &decoded)
	if decoded.Groups[c.Binding.Task].RawScans != 3 || decoded.Usage.RawScans != 3 {
		t.Fatal(decoded)
	}
}
