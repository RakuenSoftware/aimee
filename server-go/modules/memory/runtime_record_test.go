package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseRuntimeRecordReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	source, content := strings.Repeat("session-", 100), strings.Repeat("🦊", 4000)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,source_session,scope_type,scope_value,updated_at)
 VALUES('L2','fact','runtime-record',$1,$2,'project','record-private','2026-09-17T23:45:00Z') RETURNING id`, content, source).Scan(&id); err != nil {
		t.Fatal(err)
	}
	call := func(extra map[string]any, peer uint32) (map[string]any, bus.ModuleStatus) {
		t.Helper()
		args := map[string]any{"operation": "record", "id": id}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		return invokeContextCommand(t, handler, peer, bus.CommandContext{}, "runtime", string(raw))
	}
	for _, extra := range []map[string]any{nil, {"scope_context": true, "project": "record-private"}} {
		result, status := call(extra, 0)
		if status != bus.ModuleStatusOK || result["status"] != "ok" {
			t.Fatal(result, status)
		}
		record := result["memory"].(map[string]any)
		if record["content"] != content || record["source_session"] != source || record["updated_at"] != "2026-09-17T23:45:00Z" {
			t.Fatal(record)
		}
	}
	if r, status := call(map[string]any{"scope_context": true, "project": "other"}, 0); status != bus.ModuleStatusOK || r["kind"] != "not_found" {
		t.Fatal(r, status)
	}
	if _, status := call(nil, 200); status != bus.ModuleStatusInvalidRequest {
		t.Fatal(status)
	}
	if r, status := call(map[string]any{"id": 999999999}, 0); status != bus.ModuleStatusOK || r["kind"] != "not_found" {
		t.Fatal(r, status)
	}
}
