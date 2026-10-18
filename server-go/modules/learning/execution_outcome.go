package learning

import (
	"encoding/json"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
)

const EventExecution uint32 = 6147
const StageExecution uint32 = 3

// ExecutionRecord is an already persisted canonical terminal event. The host
// supplies its authenticated owner outside the JSON arguments. It is a source
// reference, never a model's claim that a task passed independent verification.
type ExecutionRecord struct {
	Version  int             `json:"version"`
	Session  string          `json:"session_id"`
	Turn     string          `json:"turn_id"`
	Sequence string          `json:"sequence"`
	EventID  string          `json:"event_id"`
	Actor    string          `json:"actor"`
	Event    json.RawMessage `json:"event"`
}

type ExecutionObservation struct {
	SchemaVersion  int             `json:"schema_version"`
	Source         ExecutionSource `json:"source"`
	ObservedStatus string          `json:"observed_status"`
	Error          string          `json:"error,omitempty"`
	Verification   string          `json:"verification"`
}

type ExecutionSource struct {
	Kind     string `json:"kind"`
	Session  string `json:"session_id"`
	Turn     string `json:"turn_id"`
	Sequence string `json:"sequence"`
	EventID  string `json:"event_id"`
	Actor    string `json:"actor"`
}

func executionObservation(record ExecutionRecord, caller bus.CommandContext) (ExecutionObservation, bool) {
	var empty ExecutionObservation
	if !caller.Authenticated || caller.UserAuthority || caller.TransportIdentity != "host:canonical-execution" || caller.Principal == "" || record.Actor != caller.Principal || record.Version != 1 {
		return empty, false
	}
	for _, value := range []string{record.Session, record.Turn, record.EventID, record.Actor} {
		if value == "" || len(value) > 256 || strings.ContainsRune(value, 0) {
			return empty, false
		}
	}
	sequence, err := strconv.ParseInt(record.Sequence, 10, 64)
	if err != nil || sequence <= 0 || strconv.FormatInt(sequence, 10) != record.Sequence || !canonical.ValidSessionEvent(record.Event) {
		return empty, false
	}
	var event struct {
		Kind    string `json:"kind"`
		Payload struct {
			Status string `json:"status"`
			Error  string `json:"error,omitempty"`
		} `json:"payload"`
	}
	if !canonical.Decode(record.Event, &event) || event.Kind != "execution" || (event.Payload.Status != "completed" && event.Payload.Status != "failed" && event.Payload.Status != "cancelled") {
		return empty, false
	}
	return ExecutionObservation{
		SchemaVersion:  1,
		Source:         ExecutionSource{Kind: "canonical_session_log", Session: record.Session, Turn: record.Turn, Sequence: record.Sequence, EventID: record.EventID, Actor: record.Actor},
		ObservedStatus: event.Payload.Status,
		Error:          event.Payload.Error,
		Verification:   "ungraded",
	}, true
}

func handleExecution(invocation bus.ModuleInvocation, raw []byte) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || len(raw) > 2<<20 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	verb, args, caller, err := bus.DecodeCommandWithContext(raw)
	var record ExecutionRecord
	if err != nil || verb != "observe" || caller == nil || !canonical.Decode(args, &record) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	observation, ok := executionObservation(record, *caller)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(map[string]any{"status": "ok", "observation": observation})
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	framed, err := bus.EncodeCommandResult(body)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return framed, bus.ModuleStatusOK
}
