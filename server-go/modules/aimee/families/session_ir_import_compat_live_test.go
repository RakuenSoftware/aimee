package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestSessionIRImportLiveCompatibilityBoundaries(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 180*time.Second)
	defer cancel()
	schema := fmt.Sprintf("ir_import_compat_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql", "schema_learning_projection_outbox.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql"} {
		raw, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(raw)); e != nil {
			t.Fatal(name, e)
		}
	}
	run := func(op uint32, f ...string) (uint32, []string, error) {
		tx, e := pool.Begin(ctx)
		if e != nil {
			return 0, nil, e
		}
		defer tx.Rollback(ctx)
		s, c, e := Sessions.Ops[op].Run(ctx, sessionIRLiveTx{tx}, f)
		if e == nil && s == store.StatusOK {
			e = tx.Commit(ctx)
		}
		return s, c, e
	}
	for _, name := range []string{"external-first", "empty-agent", "long-history", "large-tool-message"} {
		t.Run(name, func(t *testing.T) {
			if _, e := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, name); e != nil {
				t.Fatal(e)
			}
			count := 1
			if name == "long-history" {
				count = 4097
			}
			messages := make([]canonical.Message, count)
			for i := range messages {
				text := fmt.Sprintf("source message %d", i)
				messages[i] = canonical.Message{Role: "user", Blocks: []canonical.Block{{Type: "text", Text: &text}}}
			}
			large := ""
			if name == "large-tool-message" {
				large = strings.Repeat("火\t\n", 260000)
				content, _ := json.Marshal(large)
				messages = []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{{Type: "tool_use", ToolID: "call", Name: "read_file", Input: json.RawMessage(`{"path":"observed"}`)}}}, {Role: "tool", Blocks: []canonical.Block{{Type: "tool_result", ToolID: "call", Content: content}}}}
				count = len(messages)
			}
			body, e := json.Marshal(messages)
			if e != nil {
				t.Fatal(e)
			}
			agent := "agent"
			if name == "empty-agent" {
				agent = ""
			}
			if _, e = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES($1,$2,'',$3)`, name, agent, string(body)); e != nil {
				t.Fatal(e)
			}
			if name == "empty-agent" {
				if _, e = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES($1,'decoy','','[]')`, name); e != nil {
					t.Fatal(e)
				}
			}
			if name == "external-first" {
				s, _, e := run(opSessionIRHistorySave, name, "external-archive", externalSDKHistoryScope, "actor", `[{"role":"assistant","blocks":[{"type":"text","text":"independent external observation"}]}]`)
				if e != nil || s != store.StatusOK {
					t.Fatal("external archive setup", s, e)
				}
			}
			sum := sha256.Sum256(body)
			report := canonical.HistoryImportReport{Version: 1, SourceSession: name, SourceAgent: agent, SourceProvider: "", SourceSHA256: hex.EncodeToString(sum[:]), SourceFormat: "canonical", Outcome: "exact", SourceMessages: count, ImportedMessages: count, ToolStructure: "preserved"}
			encoded, _ := json.Marshal(report)
			fields := []string{name, "primary", "actor", name, agent, report.SourceSHA256, string(body), string(encoded), "model"}
			if s, _, e := run(50, fields...); e != nil || s != store.StatusOK {
				t.Fatal("canonical first import", s, e)
			}
			if s, _, e := run(50, fields...); e != nil || s != store.StatusOK {
				t.Fatal("idempotent same source", s, e)
			}
			s, c, e := run(opSessionIRHistoryLoad, name, "primary", "actor", "")
			var loaded []canonical.Message
			if e != nil || s != store.StatusOK || len(c) != 1 || !canonical.Decode([]byte(c[0]), &loaded) || len(loaded) != count {
				t.Fatal("complete primary view", s, len(loaded), e)
			}
			if name == "large-tool-message" {
				var actual string
				if json.Unmarshal(loaded[1].Blocks[0].Content, &actual) != nil || actual != large {
					t.Fatal("fragmented tool result bytes changed")
				}
				assertOriginal := func() {
					t.Helper()
					s, c, e := run(opSessionIRHistoryLoad, name, "primary", "actor", "")
					var restored []canonical.Message
					if e != nil || s != store.StatusOK || len(c) != 1 || !canonical.Decode([]byte(c[0]), &restored) || len(restored) != 2 || json.Unmarshal(restored[1].Blocks[0].Content, &actual) != nil || actual != large {
						t.Fatal("rollback changed exact imported source", s, e)
					}
				}
				for _, corruption := range []struct{ name, sql, want string }{
					{"reordered parts", `UPDATE session_ir_events SET payload=jsonb_set(payload::jsonb,'{payload,fragment,part_index}',to_jsonb(CASE WHEN (payload::jsonb->'payload'->'fragment'->>'part_index')::int=0 THEN 1 ELSE 0 END))::text WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment' AND (payload::jsonb->'payload'->'fragment'->>'part_index')::int IN (0,1)`, "invalid canonical message fragment sequence"},
					{"changed part count", `UPDATE session_ir_events SET payload=jsonb_set(payload::jsonb,'{payload,fragment,part_count}',to_jsonb((payload::jsonb->'payload'->'fragment'->>'part_count')::int+1))::text WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment' AND payload::jsonb->'payload'->'fragment'->>'part_index'='1'`, "invalid canonical message fragment sequence"},
					{"changed message identity", `UPDATE session_ir_events SET payload=jsonb_set(payload::jsonb,'{payload,fragment,message_index}',to_jsonb(2))::text WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment' AND payload::jsonb->'payload'->'fragment'->>'part_index'='1'`, "invalid canonical message fragment sequence"},
					{"part erased", `DELETE FROM session_ir_events WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment' AND payload::jsonb->'payload'->'fragment'->>'part_index'='1'`, "invalid canonical history source at sequence "},
					{"wrong complete hash", `UPDATE session_ir_events SET payload=jsonb_set(payload::jsonb,'{payload,fragment,message_sha256}',to_jsonb(repeat('0',64)))::text WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment'`, "canonical message fragment digest or content mismatch"},
				} {
					t.Run(corruption.name, func(t *testing.T) {
						tx, e := pool.Begin(ctx)
						if e != nil {
							t.Fatal(e)
						}
						defer tx.Rollback(ctx)
						if _, e = tx.Exec(ctx, corruption.sql, name); e != nil {
							t.Fatal(e)
						}
						_, _, e = Sessions.Ops[opSessionIRHistoryLoad].Run(ctx, sessionIRLiveTx{tx}, []string{name, "primary", "actor", ""})
						if e == nil || !strings.HasPrefix(e.Error(), corruption.want) {
							t.Fatal("corrupt fragment admitted or unrelated refusal", e)
						}
						if e = tx.Rollback(ctx); e != nil {
							t.Fatal(e)
						}
						assertOriginal()
					})
				}
				// A subsequent host checkpoint replaces only the current view;
				// the full protected imported source remains durably retained.
				summary := `[{"role":"user","blocks":[{"type":"text","text":"owned compacted view"}]}]`
				if s, _, e := run(opSessionIRHistorySave, name, "after-import-compaction", "primary", "actor", summary); e != nil || s != store.StatusOK {
					t.Fatal(s, e)
				}
				if s, c, e := run(opSessionIRHistoryLoad, name, "primary", "actor", ""); e != nil || s != store.StatusOK || len(c) != 1 || c[0] != summary {
					t.Fatal("compacted checkpoint reload", s, c, e)
				}
				var retained int
				if e := pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id=$1 AND payload::jsonb->'payload'->>'phase'='message_fragment'`, name).Scan(&retained); e != nil || retained < 2 {
					t.Fatal("compaction lost imported source", retained, e)
				}
			}
			if name == "external-first" {
				s, c, e = run(opSessionIRHistoryLoad, name, externalSDKHistoryScope, "actor", "")
				if e != nil || s != store.StatusOK || len(c) != 1 || !canonical.Decode([]byte(c[0]), &loaded) || len(loaded) != 1 || loaded[0].Role != "assistant" || loaded[0].Blocks[0].Text == nil || *loaded[0].Blocks[0].Text != "independent external observation" {
					t.Fatal("archive changed by primary import", s, c, e)
				}
			}
			if name == "empty-agent" {
				wrong := append([]string(nil), fields...)
				wrong[4] = "decoy"
				r := report
				r.SourceAgent = "decoy"
				raw, _ := json.Marshal(r)
				wrong[7] = string(raw)
				if s, _, e := run(50, wrong...); e != nil || s != store.StatusInvalid {
					t.Fatal("empty selector crossed to decoy source", s, e)
				}
			}
			fields[2] = "foreign"
			if s, _, e := run(50, fields...); e != nil || s != store.StatusInvalid {
				t.Fatal("foreign owner", s, e)
			}
		})
	}
}
