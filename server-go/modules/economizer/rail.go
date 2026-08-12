package economizer

import "fmt"

// Task rail — a portable plan state machine (fold §8).
//
// Ported from src/modules/economizer/task_rail.c.
//
// A small execution spine that lives OUTSIDE the prompt: the agent's plan as a
// locked list of steps with state and evidence, serializable so it survives
// folds, epoch rebirths and session boundaries. That placement is the whole
// point — a plan held only in the transcript is a plan the fold can evict, and
// an agent that loses its plan mid-task rediscovers it badly.

// StepState is a step's position in the rail.
type StepState int

const (
	StepPending  StepState = 0
	StepReserved StepState = 1 // claimed / in flight
	StepDone     StepState = 2
)

// RailStep is one planned step.
type RailStep struct {
	Title    string
	Evidence string // set when done (a handle or note); may be empty
	State    StepState
}

// TaskRail is an objective plus its ordered steps.
type TaskRail struct {
	Objective string
	// Locked is set once the rail starts: the step LIST is then fixed. Steps
	// change state, but the plan itself stops moving, so a mid-run rewrite cannot
	// quietly redefine what "done" meant.
	Locked bool
	Steps  []RailStep
}

// Start begins and locks a rail, replacing any prior state.
func (r *TaskRail) Start(objective string, titles []string) error {
	if r == nil {
		return fmt.Errorf("economizer: nil rail")
	}
	r.Objective = objective
	r.Locked = true
	r.Steps = make([]RailStep, 0, len(titles))
	for _, t := range titles {
		r.Steps = append(r.Steps, RailStep{Title: t, State: StepPending})
	}
	return nil
}

// Reserve claims a step (PENDING -> RESERVED).
//
// Only a PENDING step may be reserved: re-reserving an in-flight step would let
// two claimants believe they own it, and reserving a DONE step would silently
// reopen finished work.
func (r *TaskRail) Reserve(idx int) error {
	if r == nil || idx < 0 || idx >= len(r.Steps) {
		return fmt.Errorf("economizer: step %d out of range", idx)
	}
	if r.Steps[idx].State != StepPending {
		return fmt.Errorf("economizer: step %d is not pending", idx)
	}
	r.Steps[idx].State = StepReserved
	return nil
}

// Ack marks a step done, with optional evidence.
//
// Accepts PENDING as well as RESERVED: a step finished without being claimed
// first is still finished, and refusing it would strand the rail.
func (r *TaskRail) Ack(idx int, evidence string) error {
	if r == nil || idx < 0 || idx >= len(r.Steps) {
		return fmt.Errorf("economizer: step %d out of range", idx)
	}
	if evidence != "" {
		r.Steps[idx].Evidence = evidence
	}
	r.Steps[idx].State = StepDone
	return nil
}

// Next is the index of the first not-DONE step, or -1 when all are done.
//
// A RESERVED step counts as unfinished: it is in flight, not complete, and
// skipping past it would hide work that never landed.
func (r *TaskRail) Next() int {
	if r == nil {
		return -1
	}
	for i := range r.Steps {
		if r.Steps[i].State != StepDone {
			return i
		}
	}
	return -1
}

// DoneCount is the number of completed steps.
func (r *TaskRail) DoneCount() int {
	if r == nil {
		return 0
	}
	n := 0
	for i := range r.Steps {
		if r.Steps[i].State == StepDone {
			n++
		}
	}
	return n
}

// Serialize renders the rail as deterministic JSON, with stable key and step
// order so a persisted rail compares byte-for-byte across runs.
func (r *TaskRail) Serialize() (string, error) {
	if r == nil {
		return "", fmt.Errorf("economizer: nil rail")
	}
	root := NewObject()
	root.Set("objective", NewString(r.Objective))
	locked := &JSONValue{Kind: JSONFalse}
	if r.Locked {
		locked = &JSONValue{Kind: JSONTrue, Bool: true}
	}
	root.Set("locked", locked)
	steps := NewArray()
	for i := range r.Steps {
		s := NewObject()
		s.Set("title", NewString(r.Steps[i].Title))
		s.Set("state", NewNumber(float64(r.Steps[i].State)))
		// Evidence is omitted when absent rather than emitted empty, matching the
		// C and keeping a pending step's record free of a field implying a result.
		if r.Steps[i].Evidence != "" {
			s.Set("evidence", NewString(r.Steps[i].Evidence))
		}
		steps.Append(s)
	}
	root.Set("steps", steps)
	return PrintJSONUnformatted(root), nil
}

// RestoreRail loads a rail from JSON.
//
// ALL-OR-NOTHING: builds into a temporary and replaces *r only on complete
// success, so malformed JSON never destroys the caller's live plan. An
// out-of-range state normalizes to PENDING — the safe direction, since treating
// an unreadable state as DONE would silently drop work.
func RestoreRail(r *TaskRail, blob string) error {
	if r == nil || blob == "" {
		return fmt.Errorf("economizer: nothing to restore")
	}
	root := ParseJSON(blob)
	if root == nil || root.Kind != JSONObject {
		return fmt.Errorf("economizer: rail is not a JSON object")
	}
	steps := root.Get("steps")
	if steps != nil && !steps.IsArray() {
		return fmt.Errorf("economizer: rail steps is not an array")
	}

	var tmp TaskRail
	tmp.Objective = root.GetString("objective")
	if l := root.Get("locked"); l != nil {
		tmp.Locked = l.Kind == JSONTrue || (l.Kind == JSONNumber && l.Num != 0)
	}
	if steps != nil {
		for _, s := range steps.Items {
			if s == nil || s.Kind != JSONObject {
				continue
			}
			step := RailStep{Title: s.GetString("title"), Evidence: s.GetString("evidence")}
			if st := s.Get("state"); st != nil && st.Kind == JSONNumber {
				v := StepState(int(st.Num))
				if v == StepPending || v == StepReserved || v == StepDone {
					step.State = v
				} // else: normalizes to PENDING
			}
			tmp.Steps = append(tmp.Steps, step)
		}
	}

	*r = tmp
	return nil
}
