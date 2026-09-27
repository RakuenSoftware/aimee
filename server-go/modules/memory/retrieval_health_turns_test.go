package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"
)

func healthOwnedStart(t *testing.T, s *sourceReleaseState, task, turn string, at time.Time) (healthExecutionContext, string, commandArgs) {
	t.Helper()
	digest := strings.Repeat("a", 64)
	e := healthExecutionContext{Binding: digest, Task: releaseDigest(task), Turn: releaseDigest(turn), Source: "host_exploration_binding", QueryClass: "unclassified"}
	raw, _ := json.Marshal(map[string]any{"health_execution": e})
	args := sourceReleaseArgs(map[string]any{"request_id": turn, "principal": "alice", "project": "app", "health_turn_id": turn})
	raw, handle := s.healthExecutionTurn(raw, args, digest, at)
	var result struct {
		Execution healthExecutionContext `json:"health_execution"`
	}
	json.Unmarshal(raw, &result)
	return result.Execution, handle, args
}
func healthOwnedFinish(t *testing.T, s *sourceReleaseState, handle string, args commandArgs) {
	t.Helper()
	args["health_turn_handle"], _ = json.Marshal(handle)
	args["operation"] = json.RawMessage(`"health-turn-finish"`)
	if got := sourceReleaseCall(t, s, args); got["status"] != "ok" {
		t.Fatal(got)
	}
}
func TestHealthOwnedPredecessorRequiresCompletedDurableDispatch(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	now := time.Now()
	s := &sourceReleaseState{}
	first, h, a := healthOwnedStart(t, s, "task", "first", now)
	if first.PreviousTurn != "" || h == "" {
		t.Fatal(first, h)
	}
	s.healthTurnObserved(h, true)
	healthOwnedFinish(t, s, h, a)
	second, h2, a2 := healthOwnedStart(t, s, "task", "second", now)
	if second.PreviousTurn != first.Turn || !second.valid(strings.Repeat("a", 64)) {
		t.Fatal(second)
	}
	retry, h3, _ := healthOwnedStart(t, s, "task", "second", now)
	if h3 != h2 || retry.PreviousTurn != first.Turn {
		t.Fatal("provider retries changed predecessor")
	}
	s.healthTurnObserved(h2, true)
	s.healthTurnObserved(h3, true)
	healthOwnedFinish(t, s, h2, a2)
	other, _, _ := healthOwnedStart(t, s, "other-task", "third", now)
	if other.PreviousTurn != "" {
		t.Fatal("task boundary crossed")
	}
	restarted, _, _ := healthOwnedStart(t, &sourceReleaseState{}, "task", "restart", now)
	if restarted.PreviousTurn != "" {
		t.Fatal("restart invented predecessor")
	}
	expired, _, _ := healthOwnedStart(t, s, "task", "expired", now.Add(sourceReleaseTTL+time.Second))
	if expired.PreviousTurn != "" {
		t.Fatal("expired owner reused predecessor")
	}
}
func TestHealthOwnedPredecessorRefusesOverlapUncertaintyAndForeignFinish(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	now := time.Now()
	for _, mode := range []string{"overlap", "uncertain", "unconfirmed", "foreign"} {
		t.Run(mode, func(t *testing.T) {
			s := &sourceReleaseState{}
			_, h, a := healthOwnedStart(t, s, "task", "first", now)
			switch mode {
			case "overlap":
				e, h2, a2 := healthOwnedStart(t, s, "task", "overlapping", now)
				if e.PreviousTurn != "" {
					t.Fatal(e)
				}
				s.healthTurnObserved(h2, true)
				healthOwnedFinish(t, s, h2, a2)
				s.healthTurnObserved(h, true)
			case "uncertain":
				s.healthTurnObserved(h, false)
			case "foreign":
				bad := sourceReleaseArgs(map[string]any{"principal": "mallory", "request_id": "first", "project": "app", "operation": "health-turn-finish", "health_turn_handle": h})
				if got := sourceReleaseCall(t, s, bad); got["status"] == "ok" {
					t.Fatal("foreign finish accepted")
				}
			}
			healthOwnedFinish(t, s, h, a)
			next, _, _ := healthOwnedStart(t, s, "task", "next", now)
			if next.PreviousTurn != "" {
				t.Fatal("unproven adjacency", next)
			}
		})
	}
}
func TestHealthOwnedTurnsAreBoundedAndRollbackDisablesCapture(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	now := time.Now()
	s := &sourceReleaseState{}
	for i := 0; i < 17; i++ {
		e, h, a := healthOwnedStart(t, s, "task", fmt.Sprint(i), now)
		if i == 16 {
			if h != "" || e.PreviousTurn != "" {
				t.Fatal("capacity invented predecessor")
			}
			break
		}
		s.healthTurnObserved(h, true)
		healthOwnedFinish(t, s, h, a)
	}
	if len(s.healthTasks[releaseDigest("task")].Turns) != 16 {
		t.Fatal("turn cache unbounded")
	}
	for i := 0; i < 70; i++ {
		healthOwnedStart(t, s, fmt.Sprint(i), "turn", now)
	}
	if len(s.healthTasks) != 64 {
		t.Fatal("task cache unbounded")
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "0")
	_, handle, _ := healthOwnedStart(t, s, "disabled", "turn", now)
	if handle != "" {
		t.Fatal("disabled capture active")
	}
}
