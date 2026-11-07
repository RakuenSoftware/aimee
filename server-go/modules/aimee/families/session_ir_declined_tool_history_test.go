package families

import (
	"context"
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const declinedIntent = `{"kind":"message","message_id":"declined-model","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"declined-read","name":"read_file","input":{"path":"README.md","exact":123.123456789012345678901234567890}},{"type":"tool_use","tool_id":"declined-write","name":"write_file","input":{"path":"never-written","content":"must not execute"}}]}}`
const declinedStop = `{"kind":"delta","message_id":"declined-model","role":"assistant","payload":{"type":"TURN_STOP","stop_reason":"content_filter","raw_stop_reason":"refusal","refusal":{"category":"future_category","explanation":"provider diagnostic"}}}`
const declinedFailed = `{"kind":"execution","payload":{"status":"failed","error":"model returned a declined or incomplete canonical observation"}}`
const declinedRead = `{"kind":"message","message_id":"host-read-denial","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"declined-read","content":"Tool was not dispatched because the model response was declined or incomplete.","is_error":true}]}}`
const declinedWrite = `{"kind":"message","message_id":"host-write-denial","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"declined-write","content":"Tool was not dispatched because the model response was declined or incomplete.","is_error":true}]}}`

// A provider decline is an immutable observation. Only actual host denial
// observations close its non-dispatched calls; failed status is not a tool result.
func TestDeclinedToolIntentsReplayWithExactHostDenials(t *testing.T) {
	for _, running := range []bool{false, true} {
		t.Run(map[bool]string{false: "completed-history", true: "running-history"}[running], func(t *testing.T) {
			db := newSessDB()
			db.rows = runningHistoryRows([2]string{"declined", runningStart}, [2]string{"declined", runningUser},
				[2]string{"declined", declinedIntent}, [2]string{"declined", declinedStop}, [2]string{"declined", declinedRead},
				[2]string{"declined", declinedWrite}, [2]string{"declined", declinedFailed}, [2]string{"next", runningStart}, [2]string{"next", runningUser})
			before, _ := json.Marshal(db.rows)
			var status uint32
			var cells []string
			var err error
			if running {
				status, cells, err = sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "next"})
			} else {
				status, cells, err = sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", "next"})
			}
			if err != nil || status != store.StatusOK || len(cells) != 1 {
				t.Fatal(status, cells, err)
			}
			var history []struct {
				Role   string `json:"role"`
				Blocks []struct {
					Type    string `json:"type"`
					ID      string `json:"tool_id"`
					Content string `json:"content"`
					Error   bool   `json:"is_error"`
				} `json:"blocks"`
			}
			if err = json.Unmarshal([]byte(cells[0]), &history); err != nil {
				t.Fatal(err)
			}
			count := 4
			if running {
				count++
			}
			if len(history) != count {
				t.Fatalf("observations omitted/duplicated: %s", cells[0])
			}
			if history[0].Role != "user" || history[1].Role != "assistant" || history[2].Role != "tool" || history[3].Role != "tool" {
				t.Fatal("causal role order", cells)
			}
			ids := []string{"declined-read", "declined-write"}
			for i, id := range ids {
				call := history[1].Blocks[i]
				result := history[2+i].Blocks
				if call.Type != "tool_use" || call.ID != id || len(result) != 1 || result[0].Type != "tool_result" || result[0].ID != id || !result[0].Error || result[0].Content != "Tool was not dispatched because the model response was declined or incomplete." {
					t.Fatal("host denial binding/outcome changed", cells)
				}
			}
			if !strings.Contains(cells[0], "123.123456789012345678901234567890") {
				t.Fatal("model intent number changed", cells)
			}
			after, _ := json.Marshal(db.rows)
			if !reflect.DeepEqual(before, after) {
				t.Fatal("source observations mutated")
			}
			if !strings.Contains(string(after), `\"raw_stop_reason\":\"refusal\"`) || !strings.Contains(string(after), `\"status\":\"failed\"`) {
				t.Fatal("original terminal refusal lost", string(after))
			}
		})
	}
}

func TestFailedToolIntentHistoryDoesNotInventDenialOrSuccess(t *testing.T) {
	db := newSessDB()
	db.rows = runningHistoryRows([2]string{"declined", runningStart}, [2]string{"declined", runningUser}, [2]string{"declined", declinedIntent}, [2]string{"declined", declinedStop}, [2]string{"declined", declinedFailed})
	status, cells, err := sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", ""})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	var history []json.RawMessage
	if json.Unmarshal([]byte(cells[0]), &history) != nil || len(history) != 2 || strings.Contains(cells[0], `"type":"tool_result"`) {
		t.Fatal("projector manufactured tool outcome", cells)
	}
}
