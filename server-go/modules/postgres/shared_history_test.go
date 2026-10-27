package postgres

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/db"
	canonical "github.com/JBailes/aimee/server-go/ir"
	domain "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
	"strings"
	"testing"
	"time"
)

type historyWireCaller struct {
	sharedStoreCaller
	t       *testing.T
	pool    *pgxpool.Pool
	pages   int
	checked bool
}

func (c *historyWireCaller) Call(ctx context.Context, kind, stage uint32, id uint64, deadline time.Duration, body []byte) ([]byte, error) {
	reply, err := c.sharedStoreCaller.Call(ctx, kind, stage, id, deadline, body)
	if len(reply) > int(ring.ModuleMessageMaxBody) {
		return nil, fmt.Errorf("test transport refused oversized production SQL reply: %d", len(reply))
	}
	if bytes.Contains(body, []byte("WITH owned AS MATERIALIZED")) {
		c.pages++
		if !c.checked {
			c.checked = true
			probe, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
			defer cancel()
			if _, e := c.pool.Exec(probe, `UPDATE server_sessions SET principal=principal WHERE id='history'`); e == nil {
				c.t.Fatal("source owner lock was released between history pages")
			}
		}
	}
	return reply, err
}

// Traverse the production Go SQL wire. A direct pgx fixture would miss both
// reply ceilings even though the final model checkpoint is one small message.
func TestSharedCanonicalHistoryPagesEscapedArchiveAndLongLog(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	for _, sql := range []string{
		`CREATE TABLE server_sessions(id text PRIMARY KEY,principal text NOT NULL)`,
		`CREATE TABLE session_ir_events(session_id text REFERENCES server_sessions(id),sequence bigint,event_id text,turn_id text,actor text,payload text CHECK(octet_length(payload)<=1048576),PRIMARY KEY(session_id,sequence))`,
		`INSERT INTO server_sessions VALUES('history','actor')`,
	} {
		if _, err := pool.Exec(ctx, sql); err != nil {
			t.Fatal(err)
		}
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(context.Background())
	sequence := 0
	insert := func(turn string, value any) {
		t.Helper()
		raw, e := json.Marshal(value)
		if e != nil {
			t.Fatal(e)
		}
		sequence++
		if _, e = tx.Exec(ctx, `INSERT INTO session_ir_events VALUES('history',$1,$2,$3,'actor',$4)`, sequence, fmt.Sprint(sequence), turn, string(raw)); e != nil {
			t.Fatal(e)
		}
	}
	// JSON escaping expands this valid3MiB source beyond18MiB of events.
	chunk := strings.Repeat("\x01", 65536)
	for i := 0; i < 48; i++ {
		insert("archive", map[string]any{"kind": "history", "payload": map[string]any{"scope": "legacy-archive", "phase": "archive", "index": i, "archive_text": chunk}})
	}
	for i := 0; i < 4100; i++ {
		insert("old", map[string]any{"kind": "history", "payload": map[string]any{"scope": "external-sdk:codex", "phase": "message", "index": i, "message": map[string]any{"role": "user", "blocks": []any{map[string]any{"type": "text", "text": "archived observation"}}}}})
	}
	insert("old", map[string]any{"kind": "history", "payload": map[string]any{"scope": "external-sdk:codex", "phase": "commit", "count": 4100}})
	insert("current", map[string]any{"kind": "history", "payload": map[string]any{"scope": "primary", "phase": "message", "index": 0, "message": map[string]any{"role": "user", "blocks": []any{map[string]any{"type": "text", "text": "exact current checkpoint"}}}}})
	insert("current", map[string]any{"kind": "history", "payload": map[string]any{"scope": "primary", "phase": "commit", "count": 1}})
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	h := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &historyWireCaller{sharedStoreCaller: sharedStoreCaller{h.handle}, t: t, pool: pool}
	store, err := db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := store.Query(ctx, `SELECT payload FROM session_ir_events WHERE sequence<=48 ORDER BY sequence`)
	if err == nil {
		rows.Close()
		t.Fatal("fixture did not exceed production reply budget")
	}
	request, err := wire.EncodeFields(33, []string{"history", "primary", "actor", ""})
	if err != nil {
		t.Fatal(err)
	}
	reply, status := families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
	result, cells, err := wire.DecodeFields(reply)
	if status != ring.ModuleStatusOK || err != nil || result != domain.StatusOK || len(cells) != 1 {
		t.Fatalf("history refused: transport=%d result=%d err=%v cells=%v", status, result, err, cells)
	}
	messages, err := canonical.DecodeMessages([]byte(cells[0]))
	if err != nil || len(messages) != 1 || !strings.Contains(cells[0], "exact current checkpoint") || strings.Contains(cells[0], "archived observation") {
		t.Fatal("history projection changed", cells, err)
	}
	if caller.pages < 18 || !caller.checked {
		t.Fatal("fixture did not traverse bounded source pages", caller.pages)
	}
	// Browse uses op29's separate presentation budget. Visit every source event
	// through actual bounded wire replies, never skipping a non-emitted candidate.
	after := 0
	for after < sequence {
		pageRequest, _ := wire.EncodeFields(29, []string{"history", fmt.Sprint(after), "128", "actor"})
		pageReply, pageStatus := families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, pageRequest)
		pageResult, pageCells, pageErr := wire.DecodeFields(pageReply)
		if pageStatus != ring.ModuleStatusOK || pageErr != nil || pageResult != domain.StatusOK || len(pageCells) != 1 {
			t.Fatal("browse refused", after, pageStatus, pageResult, pageErr)
		}
		var observations []struct {
			Sequence string          `json:"sequence"`
			Event    json.RawMessage `json:"event"`
		}
		if e := json.Unmarshal([]byte(pageCells[0]), &observations); e != nil || len(observations) == 0 {
			t.Fatal("browse lost source", after, e)
		}
		if len(pageCells[0]) > 3<<20 {
			t.Fatal("presentation page exceeded budget")
		}
		for _, observation := range observations {
			after++
			if observation.Sequence != fmt.Sprint(after) {
				t.Fatal("browse skipped or duplicated source", after, observation.Sequence)
			}
			if after <= 48 {
				var event struct {
					Payload struct {
						Text string `json:"archive_text"`
					} `json:"payload"`
				}
				if e := json.Unmarshal(observation.Event, &event); e != nil || event.Payload.Text != chunk {
					t.Fatal("archive bytes changed", after, e)
				}
			}
		}
	}
	request, _ = wire.EncodeFields(33, []string{"history", "primary", "other", ""})
	reply, status = families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
	result, cells, err = wire.DecodeFields(reply)
	if status != ring.ModuleStatusOK || err != nil || result != domain.StatusMissing || len(cells) != 0 {
		t.Fatal("foreign history escaped", status, result, cells, err)
	}
}

func TestSharedCanonicalLargeImportUsesBoundedOperationBudget(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	store := sharedTestStore(t, pool)
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('import','actor')`); err != nil {
		t.Fatal(err)
	}
	messages := make([]json.RawMessage, 4097)
	for i := range messages {
		messages[i] = json.RawMessage(fmt.Sprintf(`{"role":"user","blocks":[{"type":"text","text":"original-%d"}]}`, i))
	}
	raw, err := json.Marshal(messages)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES('import','','anthropic',$1)`, string(raw)); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(raw)
	report, err := json.Marshal(canonical.HistoryImportReport{Version: 1, SourceFormat: "canonical", Outcome: "exact", SourceSHA256: hex.EncodeToString(hash[:]), SourceSession: "import", SourceAgent: "", SourceProvider: "anthropic", SourceMessages: len(messages), ImportedMessages: len(messages), ToolStructure: "preserved"})
	if err != nil {
		t.Fatal(err)
	}
	request, err := wire.EncodeFields(50, []string{"import", "primary", "actor", "import", "", hex.EncodeToString(hash[:]), string(raw), string(report), "model"})
	if err != nil {
		t.Fatal(err)
	}
	reply, status := families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
	result, cells, err := wire.DecodeFields(reply)
	if status != ring.ModuleStatusOK || err != nil || result != domain.StatusOK || len(cells) != 1 || cells[0] != "imported" {
		t.Fatalf("large import failed: transport=%d result=%d err=%v disposition=%v", status, result, err, cells)
	}
	request, _ = wire.EncodeFields(33, []string{"import", "primary", "actor", ""})
	reply, status = families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
	result, cells, err = wire.DecodeFields(reply)
	if status != ring.ModuleStatusOK || err != nil || result != domain.StatusOK || len(cells) != 1 {
		t.Fatal(status, result, cells, err)
	}
	var recovered []json.RawMessage
	if err = json.Unmarshal([]byte(cells[0]), &recovered); err != nil || len(recovered) != len(messages) {
		t.Fatal("import truncated", len(recovered), err)
	}
	for i := range messages {
		if !bytes.Equal(messages[i], recovered[i]) {
			t.Fatal("original canonical message changed", i)
		}
	}
}
