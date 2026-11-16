package families

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// A rejected continuation is retained for operator review in an execution
// diagnostic, never a new user instruction that a later model could execute.
func TestRejectedSteeringDiagnosticDoesNotEnterModelHistory(t *testing.T) {
	const rejected = "DO_NOT_EXECUTE_REJECTED_STEERING"
	failed, err := json.Marshal(map[string]any{"kind": "execution", "payload": map[string]any{
		"status": "failed", "error": "Not dispatched; no model or tool executed. Observed submitted input: " + rejected,
	}})
	if err != nil {
		t.Fatal(err)
	}
	for _, running := range []bool{false, true} {
		db := newSessDB()
		events := [][2]string{{"old", runningStart}, {"old", runningUser}, {"old", runningDone},
			{"rejected-admission", runningStart}, {"rejected-admission", string(failed)}, {"active", runningStart}}
		db.rows = runningHistoryRows(events...)
		var status uint32
		var cells []string
		if running {
			status, cells, err = sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
		} else {
			status, cells, err = sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", "active"})
		}
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatal(running, status, cells, err)
		}
		var messages []json.RawMessage
		if json.Unmarshal([]byte(cells[0]), &messages) != nil || len(messages) != 1 ||
			!strings.Contains(cells[0], "current user") || strings.Contains(cells[0], rejected) {
			t.Fatal("failed admission became model context", running, cells)
		}
		if !strings.Contains(events[4][1], rejected) {
			t.Fatal("source diagnostic lost rejected input")
		}
	}
}
