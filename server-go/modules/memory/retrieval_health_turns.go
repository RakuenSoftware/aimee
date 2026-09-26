package memory

import (
	"encoding/json"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// Optional process-local ownership, never a global "last request" heuristic.
// Durable receipts retain links already issued. Restart/expiry loses knowledge,
// so the next turn has no predecessor. The caller holds the receipt owner mutex.
type healthOwnedTurn struct {
	Handle    string
	Binding   string
	Turn      string
	Previous  string
	Active    bool
	Delivered bool
	Pending   int
	Ambiguous bool
}
type healthTaskTurns struct {
	Turns   map[string]*healthOwnedTurn
	Last    string
	Expires time.Time
}

func (s *sourceReleaseState) healthExecutionTurn(metadata json.RawMessage, args commandArgs, digest string, now time.Time) (json.RawMessage, string) {
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" || args.stringOr("health_turn_id", "") == "" {
		return metadata, ""
	}
	var fields map[string]json.RawMessage
	var execution healthExecutionContext
	if json.Unmarshal(metadata, &fields) != nil || json.Unmarshal(fields["health_execution"], &execution) != nil || !execution.valid(digest) {
		return metadata, ""
	}
	if s.healthTasks == nil {
		s.healthTasks = map[string]*healthTaskTurns{}
	}
	for key, task := range s.healthTasks {
		if !now.Before(task.Expires) {
			delete(s.healthTasks, key)
		}
	}
	task := s.healthTasks[execution.Task]
	if task == nil {
		if len(s.healthTasks) >= 64 {
			return metadata, ""
		}
		task = &healthTaskTurns{Turns: map[string]*healthOwnedTurn{}, Expires: now.Add(sourceReleaseTTL)}
		s.healthTasks[execution.Task] = task
	}
	turn := task.Turns[execution.Turn]
	if turn == nil {
		// Never evict an active turn and then certify adjacency across it.
		if len(task.Turns) >= 16 {
			task.Last = ""
			for _, t := range task.Turns {
				t.Ambiguous = true
			}
			return metadata, ""
		}
		handle, err := releaseToken()
		if err != nil {
			return metadata, ""
		}
		turn = &healthOwnedTurn{Handle: handle, Binding: releaseBinding(args), Turn: execution.Turn, Previous: task.Last, Active: true}
		for _, active := range task.Turns {
			if active.Active {
				active.Ambiguous = true
				turn.Ambiguous = true
				turn.Previous = ""
				task.Last = ""
			}
		}
		task.Turns[turn.Turn] = turn
	}
	if !turn.Active || turn.Binding != releaseBinding(args) {
		return metadata, ""
	}
	task.Expires = now.Add(sourceReleaseTTL)
	turn.Pending++
	execution.PreviousTurn = turn.Previous
	execution.PredecessorSource = "receipt_owner_completed_native_turn_v1"
	fields["health_execution"], _ = json.Marshal(execution)
	encoded, err := json.Marshal(fields)
	if err != nil || len(encoded) > 12000 {
		turn.Ambiguous = true
		task.Last = ""
		return metadata, turn.Handle
	}
	return encoded, turn.Handle
}

func (s *sourceReleaseState) healthTurnObserved(handle string, acknowledged bool) {
	if handle == "" {
		return
	}
	for _, task := range s.healthTasks {
		for _, turn := range task.Turns {
			if turn.Handle == handle && turn.Active {
				if turn.Pending > 0 {
					turn.Pending--
				}
				if acknowledged {
					turn.Delivered = true
				} else {
					turn.Ambiguous = true
					task.Last = ""
				}
				return
			}
		}
	}
}
func (s *sourceReleaseState) healthTurnFinish(args commandArgs) ([]byte, bus.ModuleStatus) {
	handle := args.stringOr("health_turn_handle", "")
	if !releaseTokenValid(handle) {
		return commandResult(commandError("unavailable", "health turn owner unavailable"))
	}
	for _, task := range s.healthTasks {
		for _, turn := range task.Turns {
			if turn.Handle != handle {
				continue
			}
			if turn.Binding != releaseBinding(args) {
				return commandResult(commandError("unavailable", "health turn owner unavailable"))
			}
			if turn.Active {
				turn.Active = false
				if turn.Pending != 0 {
					turn.Ambiguous = true
				}
				if turn.Ambiguous {
					task.Last = ""
				} else if turn.Delivered {
					task.Last = turn.Turn
				}
			}
			return commandResult(map[string]any{"status": "ok"})
		}
	}
	return commandResult(commandError("unavailable", "health turn owner unavailable"))
}
