package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

// Both fixtures call this against their production methods and public/host
// routes. The transaction clock remains fixed while the session timezone varies.
func exerciseDeadlineReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler, kind string) {
	t.Helper()
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT deadline_replay; SET LOCAL TIME ZONE 'Asia/Tokyo'`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT deadline_replay; RELEASE SAVEPOINT deadline_replay`) }()
	table, fields, values, state := "epistemic_directives", "question,topic,cause,priority", "'deadline expired','deadline','user_follow_up',100", "open"
	if kind == "prospective" {
		table, fields, values, state = "prospective_memories", "trigger_text,action_text", "'deadline','deadline expired'", "armed"
	}
	exec(`INSERT INTO ` + table + `(` + fields + `,valid_until,created_at) SELECT ` + values + `,
 to_char((CURRENT_TIMESTAMP-interval '1 minute') AT TIME ZONE 'Etc/GMT-14','YYYY-MM-DD"T"HH24:MI:SS.US')||'+14:00','2099-01-01T00:00:00Z'
 FROM generate_series(1,40)`)
	var boundary int64
	if err := tx.QueryRow(ctx, `INSERT INTO `+table+`(`+fields+`,valid_until,created_at) VALUES (`+values+`,CURRENT_TIMESTAMP::text,'2099-01-02T00:00:00Z') RETURNING id`).Scan(&boundary); err != nil {
		t.Fatal(err)
	}
	if kind == "directive" {
		values = "'deadline eligible','deadline','user_follow_up',90"
	} else {
		values = "'deadline','deadline eligible'"
	}
	var good int64
	if err := tx.QueryRow(ctx, `INSERT INTO `+table+`(`+fields+`,valid_until) VALUES (`+values+`,
 to_char((CURRENT_TIMESTAMP+interval '1 minute') AT TIME ZONE 'Etc/GMT+12','YYYY-MM-DD"T"HH24:MI:SS.US')||'-12:00') RETURNING id`).Scan(&good); err != nil {
		t.Fatal(err)
	}
	client := clientForHandler(t, handler)
	current := func() {
		t.Helper()
		for _, operation := range []string{kind + "-current", kind + "-match"} {
			reply, err := client.Data(ctx, 73, DataRequest{Operation: operation, Query: "deadline", Limit: 1})
			if err != nil {
				t.Fatal(operation, err)
			}
			if kind == "directive" {
				if len(reply.Directives) != 1 || reply.Directives[0].ID != good {
					t.Fatal(operation, reply.Directives)
				}
			} else if len(reply.Prospectives) != 1 || reply.Prospectives[0].ID != good {
				t.Fatal(operation, reply.Prospectives)
			}
		}
		reply := runHostRuntime(t, handler, `{"operation":"`+kind+`-briefing","limit":1}`)
		block, _ := reply["block"].(string)
		if !strings.Contains(block, "deadline eligible") || strings.Contains(block, "deadline expired") {
			t.Fatal("briefing expiry", reply)
		}
	}
	current()
	// Operator lifecycle views intentionally retain unswept rows.
	reply, err := client.Data(ctx, 73, DataRequest{Operation: kind + "-list", State: state, Limit: 256})
	if err != nil {
		t.Fatal(err)
	}
	count := len(reply.Directives)
	if kind == "prospective" {
		count = len(reply.Prospectives)
	}
	if count != 42 {
		t.Fatal("operator view lost stored state", count)
	}
	swept, err := client.Data(ctx, 73, DataRequest{Operation: kind + "-sweep"})
	if err != nil {
		t.Fatal(err)
	}
	count = swept.Expired
	if kind == "prospective" {
		count = swept.ProspectiveExpired
	}
	if count != 41 {
		t.Fatal("half-open sweep boundary", swept)
	}
	var boundaryState string
	if err := tx.QueryRow(ctx, `SELECT state FROM `+table+` WHERE id=$1`, boundary).Scan(&boundaryState); err != nil || boundaryState != "expired" {
		t.Fatal(boundaryState, err)
	}
	current()
	for _, bad := range []string{"now", "infinity", "2026-02-30T00:00:00Z"} {
		exec(`SAVEPOINT malformed_deadline`)
		exec(`UPDATE `+table+` SET valid_until=$1 WHERE id=$2`, bad, good)
		for _, operation := range []string{kind + "-current", kind + "-match", kind + "-sweep"} {
			raw, _ := json.Marshal(DataRequest{Operation: operation, Query: "deadline", Limit: 1})
			_, status := handler(bus.ModuleInvocation{StageID: StageData}, raw)
			if status == bus.ModuleStatusOK {
				t.Fatal("malformed deadline accepted", operation, bad)
			}
		}
		exec(`ROLLBACK TO SAVEPOINT malformed_deadline; RELEASE SAVEPOINT malformed_deadline`)
		current()
	}
}
