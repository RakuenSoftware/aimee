package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/jackc/pgx/v5"
)

func TestMutationAuditFields(t *testing.T) {
	secret := "DO-NOT-PUBLISH-alice@example.com"
	req := DataRequest{Operation: "insert-epistemic", Key: secret, Content: secret, Reason: secret, SessionID: "session\n1"}
	response := DataResponse{Records: []Record{{ID: math.MaxInt64, Tier: "L2", Kind: "fact", Key: secret, Content: secret, Confidence: .88}}}
	a, ok := mutationAudit(req, response, bus.ModuleStatusOK)
	if !ok || a.Tool != "memory.insert" || a.TaskID != math.MaxInt64 || a.Mode != "L2" || a.Reason != "conf=0.88" || a.Verdict != "ok" || !strings.HasPrefix(a.Command, "mk:") || len(a.Command) != 15 {
		t.Fatal(a, ok)
	}
	body := audit.EncodeAction(a)
	for _, raw := range []string{secret, "alice", "example.com", "fact", "\n"} {
		if bytes.Contains(body, []byte(raw)) {
			t.Fatal("unsanitized action", raw)
		}
	}
	// Different full identities remain distinct, including beyond the old fixed
	// native fingerprint buffer; neither identity becomes audit text.
	other := response
	other.Records = append([]Record(nil), response.Records...)
	other.Records[0].Key = secret + "-other"
	b, _ := mutationAudit(req, other, bus.ModuleStatusOK)
	if a.Command == b.Command {
		t.Fatal("fingerprint collision")
	}
	failed, _ := mutationAudit(req, response, bus.ModuleStatusInternal)
	if failed.Verdict != "fail" || failed.TaskID != 0 || failed.Command != "" || failed.Mode != "" {
		t.Fatal("uncommitted row leaked", failed)
	}
	zero, denied := 0, -1
	for _, c := range []struct {
		operation string
		response  DataResponse
		want      string
		id        int64
	}{
		{"update-as", DataResponse{Code: &zero, IDs: []int64{43}}, "ok", 43},
		{"update-as", DataResponse{Code: &denied, IDs: []int64{0}}, "fail", 42},
		{"reject", DataResponse{Updated: true}, "ok", 42}, {"restore", DataResponse{}, "fail", 42},
		{"delete-as", DataResponse{Deleted: true}, "ok", 42}, {"delete", DataResponse{}, "fail", 42},
	} {
		got, ok := mutationAudit(DataRequest{Operation: c.operation, ID: 42}, c.response, bus.ModuleStatusOK)
		if !ok || got.Verdict != c.want || got.TaskID != c.id {
			t.Fatal(c, got)
		}
	}
	if _, ok := mutationAudit(DataRequest{Operation: "get"}, DataResponse{}, bus.ModuleStatusOK); ok {
		t.Fatal("read emitted mutation")
	}
}

func TestMutationAuditDeliveryFailureIsVisibleAndContentFree(t *testing.T) {
	var logs bytes.Buffer
	old := log.Writer()
	log.SetOutput(&logs)
	defer log.SetOutput(old)
	calls := 0
	publishMutationAudit(func(ctx context.Context, a audit.Action) error {
		calls++
		if _, ok := ctx.Deadline(); !ok {
			t.Fatal("unbounded delivery")
		}
		return errors.New("transport message containing SENSITIVE-DETAIL")
	}, DataRequest{Operation: "delete", ID: 42}, DataResponse{Deleted: true}, bus.ModuleStatusOK)
	if calls != 1 || !strings.Contains(logs.String(), "memory.delete id=42 delivery failed") || strings.Contains(logs.String(), "SENSITIVE") {
		t.Fatal(calls, logs.String())
	}
}

func exerciseMutationAuditReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT mutation_audit_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT mutation_audit_replay; RELEASE SAVEPOINT mutation_audit_replay`) }()
	var actions []audit.Action
	observed := *backend
	observed.auditAction = func(ctx context.Context, a audit.Action) error {
		// The audit callback runs after the request's commit/rollback, so the
		// outer fixture transaction is usable even when a constraint aborted SQL.
		var count int
		if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM memories WHERE key='audit-failure'`).Scan(&count); err != nil {
			t.Fatal("audit preceded rollback", err)
		}
		if a.Verdict == "fail" && count != 0 {
			t.Fatal("partial mutation reported", count)
		}
		actions = append(actions, a)
		return nil
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &observed))
	client := clientForHandler(t, handler)
	caller := bus.CommandContext{Authenticated: true, Principal: "operator:audit", UserAuthority: true, TransportIdentity: "cert:audit"}
	call := func(verb, args string) map[string]any {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, caller, verb, args)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		return r
	}
	stored := call("store", `{"tier":"L2","key":"audit-fixture","content":"a useful note","authority":"user","scope_context":true,"project":"audit-project"}`)
	if stored["status"] != "ok" || len(actions) != 1 || actions[0].Verdict != "ok" || actions[0].Tool != "memory.insert" {
		t.Fatal(stored, actions)
	}
	id := int64(stored["id"].(float64))
	for _, verb := range []string{"update", "reject", "restore", "delete"} {
		r := call(verb, fmt.Sprintf(`{"id":%d,"content":"changed note","authority":"user","scope_context":true,"project":"audit-project"}`, id))
		if r["status"] != "ok" || actions[len(actions)-1].Tool != "memory."+verb || actions[len(actions)-1].Verdict != "ok" {
			t.Fatal(verb, r, actions)
		}
	}
	if len(actions) != 5 {
		t.Fatal("duplicate events", actions)
	}
	call("get", fmt.Sprintf(`{"id":%d,"scope_context":true,"project":"audit-project"}`, id))
	if len(actions) != 5 {
		t.Fatal("read emitted action")
	}
	call("reject", fmt.Sprintf(`{"id":%d,"scope_context":true,"project":"audit-project"}`, id))
	if len(actions) != 6 || actions[5].Verdict != "fail" {
		t.Fatal("missing refused action", actions)
	}
	exec(`SAVEPOINT audit_failure; RESET ROLE;
 ALTER TABLE memories ADD CONSTRAINT audit_failure CHECK(key<>'audit-failure');
 SET LOCAL ROLE aimee_store_runtime`)
	result := runPublicCommand(t, client, "store", `{"key":"audit-failure","content":"a note","scope_context":true,"project":"audit-project"}`)
	if result["kind"] != "unavailable" || len(actions) != 7 || actions[6].Verdict != "fail" || actions[6].TaskID != 0 {
		t.Fatal(result, actions)
	}
	exec(`ROLLBACK TO SAVEPOINT audit_failure; RELEASE SAVEPOINT audit_failure`)
	// Notification failure cannot turn a committed write into a reported failure
	// that encourages a duplicate retry. The SQL WORM row still commits with it.
	observed.auditAction = func(context.Context, audit.Action) error { return errors.New("offline") }
	stored = call("store", `{"key":"audit-committed","content":"a note","scope_context":true,"project":"audit-project"}`)
	if stored["status"] != "ok" {
		t.Fatal(stored)
	}
	raw, _ := json.Marshal(actions)
	if strings.Contains(string(raw), "changed note") || strings.Contains(string(raw), "audit-fixture") {
		t.Fatal("content/raw identity reached action")
	}
}
