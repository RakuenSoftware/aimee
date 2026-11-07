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
	"unicode/utf8"
)

type legacySourceLockCaller struct {
	sharedStoreCaller
	check func()
}

func (c *legacySourceLockCaller) Call(ctx context.Context, kind, stage uint32, id uint64, deadline time.Duration, request []byte) ([]byte, error) {
	reply, err := c.sharedStoreCaller.Call(ctx, kind, stage, id, deadline, request)
	if c.check != nil && bytes.Contains(request, []byte("CROSS JOIN generate_series($4::bigint,$5::bigint)")) {
		check := c.check
		c.check = nil
		check()
	}
	return reply, err
}

func TestSharedLargeLegacySourceReadsAndImport(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	store := sharedTestStore(t, pool)
	history, err := families.Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, migration := range history {
		if migration.Version >= 20 {
			break
		}
		if err = store.Migrate(ctx, domain.MigrationRequest{Owner: migration.Owner, Version: migration.Version, Checksum: migration.Checksum, Statements: migration.Statements}); err != nil {
			t.Fatal(err)
		}
	}
	call := func(op uint32, fields ...string) (uint32, []string, error) {
		request, err := wire.EncodeFields(op, fields)
		if err != nil {
			return 0, nil, err
		}
		reply, status := families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
		if status != ring.ModuleStatusOK {
			return 0, nil, fmt.Errorf("transport=%d", status)
		}
		return wire.DecodeFields(reply)
	}
	text := strings.Repeat("x", 1200*1024)
	content, _ := json.Marshal(text)
	canonicalMessages := []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{{Type: "tool_use", ToolID: "call", Name: "read_file", Input: json.RawMessage(`{"path":"original"}`)}}}, {Role: "tool", Blocks: []canonical.Block{{Type: "tool_result", ToolID: "call", Content: content}}}}
	canonicalRaw, _ := json.Marshal(canonicalMessages)
	cases := []struct {
		id, provider, raw string
		messages          []canonical.Message
	}{{"canonical", "anthropic", string(canonicalRaw), canonicalMessages}, {"archive", "tmux-cli", strings.Repeat("火\t\n", 630000), nil}}
	// Retain genuine pre20 source rows, then run every production migration.
	for _, test := range cases {
		if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, test.id); err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES($1,'agent',$2,$3)`, test.id, test.provider, test.raw); err != nil {
			t.Fatal(err)
		}
	}
	if err = families.ApplySchema(ctx, store); err != nil {
		t.Fatal("prelimit retained source upgrade", err)
	}
	if err = families.ApplySchema(ctx, store); err != nil {
		t.Fatal("deferred20 exactplan replay", err)
	}
	handler := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &legacySourceLockCaller{sharedStoreCaller: sharedStoreCaller{handler.handle}}
	store, err = db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range cases {
		t.Run(test.id, func(t *testing.T) {
			caller.check = func() {
				probe, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
				defer cancel()
				if _, e := pool.Exec(probe, `UPDATE primary_sessions SET messages_json='[]' WHERE session_id=$1`, test.id); e == nil {
					t.Fatal("legacy source lock released between chunk reads")
				}
			}
			status, cells, err := call(15, test.id)
			if err != nil || status != domain.StatusOK || len(cells) != 6 || cells[5] != test.raw {
				t.Errorf("latest exactsource: status=%d cells=%d err=%v", status, len(cells), err)
			}
			sum := sha256.Sum256([]byte(test.raw))
			digest := hex.EncodeToString(sum[:])
			report := canonical.HistoryImportReport{Version: 1, SourceSession: test.id, SourceAgent: "agent", SourceProvider: test.provider, SourceSHA256: digest, SourceFormat: "canonical", Outcome: "exact", SourceMessages: len(test.messages), ImportedMessages: len(test.messages), ToolStructure: "preserved"}
			if test.messages == nil {
				report.SourceFormat = "tmux_text"
				report.Outcome = "archived"
				report.ToolStructure = "unrecoverable"
				for raw := test.raw; len(raw) > 0; {
					n := len(raw)
					if n > 65536 {
						n = 65536
						for n > 0 && !utf8.RuneStart(raw[n]) {
							n--
						}
					}
					report.ArchiveChunks++
					raw = raw[n:]
				}
			}
			history := []byte("[]")
			if test.messages != nil {
				history = canonicalRaw
			}
			encoded, _ := json.Marshal(report)
			status, cells, err = call(50, test.id, "primary", "actor", test.id, "agent", digest, string(history), string(encoded), "model")
			if err != nil || status != domain.StatusOK || len(cells) != 1 || cells[0] != "imported" {
				t.Fatalf("large source verification: status=%d cells=%d err=%v", status, len(cells), err)
			}
			status, cells, err = call(33, test.id, "primary", "actor", "")
			if test.messages == nil {
				if err != nil || status != domain.StatusMissing {
					t.Fatal("archive entered model history", status, err)
				}
			} else {
				if err != nil || status != domain.StatusOK || len(cells) != 1 || cells[0] != string(canonicalRaw) {
					t.Fatal("large canonical bytes changed", status, err)
				}
			}
		})
	}
}

func TestSharedLegacySourceMigrationPlansKeepExactChecksums(t *testing.T) {
	for _, scenario := range []string{"fresh", "installed-original20", "unknown20"} {
		t.Run(scenario, func(t *testing.T) {
			pool := sharedTestDatabase(t)
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			store := sharedTestStore(t, pool)
			history, err := families.Migrations()
			if err != nil {
				t.Fatal(err)
			}
			if scenario != "fresh" {
				for _, migration := range history {
					if migration.Version >= 50 {
						break
					}
					if err = store.Migrate(ctx, domain.MigrationRequest{Owner: migration.Owner, Version: migration.Version, Checksum: migration.Checksum, Statements: migration.Statements}); err != nil {
						t.Fatal(err)
					}
				}
			}
			if scenario == "unknown20" {
				if _, err = pool.Exec(ctx, `UPDATE schema_migrations SET checksum=$1 WHERE owner='db1' AND version=20`, strings.Repeat("0", 64)); err != nil {
					t.Fatal(err)
				}
				if err = families.ApplySchema(ctx, store); err == nil {
					t.Fatal("unknown historical plan admitted")
				}
				var after int
				if err = pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE owner='db1' AND version>=50`).Scan(&after); err != nil || after != 0 {
					t.Fatal("later migration ran after unknown plan", after, err)
				}
				if _, err = pool.Exec(ctx, `UPDATE schema_migrations SET checksum=$1 WHERE owner='db1' AND version=20`, history[19].Checksum); err != nil {
					t.Fatal(err)
				}
			}
			if err = families.ApplySchema(ctx, store); err != nil {
				t.Fatal(err)
			}
			if err = families.ApplySchema(ctx, store); err != nil {
				t.Fatal("known plan replay", err)
			}
			var actual string
			var validated bool
			if err = pool.QueryRow(ctx, `SELECT pg_get_constraintdef(oid),convalidated FROM pg_constraint WHERE conname='primary_sessions_messages_json_fits_wire' AND conrelid='primary_sessions'::regclass`).Scan(&actual, &validated); err != nil || !validated || !strings.Contains(actual, "16777216") {
				t.Fatal("source constraint not validated", actual, validated, err)
			}
			var recorded string
			if err = pool.QueryRow(ctx, `SELECT checksum FROM schema_migrations WHERE owner='db1' AND version=20`).Scan(&recorded); err != nil {
				t.Fatal(err)
			}
			if scenario == "fresh" {
				if recorded == history[19].Checksum {
					t.Fatal("changed SQL claimed original checksum")
				}
			} else if recorded != history[19].Checksum {
				t.Fatal("original installed checksum replaced")
			}
		})
	}
}

func TestSharedLegacyGetterExactReplyEnvelopeBoundary(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	store := sharedTestStore(t, pool)
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	const session = "边界"
	const agent = "agent"
	const provider = "tmux-cli"
	if _, err := pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json,created_at,updated_at) VALUES($1,$2,$3,'','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z')`, session, agent, provider); err != nil {
		t.Fatal(err)
	}
	for _, excess := range []int{0, 1} {
		t.Run(fmt.Sprintf("extra%d", excess), func(t *testing.T) {
			capacity := int(ring.ModuleMessageMaxBody) - 8 - 4*6 - len(session) - len(agent) - len(provider) - 2*19
			fields := []string{session}
			size := capacity + excess
			raw := strings.Repeat("火", size/3) + strings.Repeat("x", size%3)
			if _, err := pool.Exec(ctx, `UPDATE primary_sessions SET messages_json=$1 WHERE session_id=$2`, raw, session); err != nil {
				t.Fatal(err)
			}
			request, err := wire.EncodeFields(15, fields)
			if err != nil {
				t.Fatal(err)
			}
			reply, transport := families.Sessions.Handler(store)(ring.ModuleInvocation{StageID: families.StageSessions}, request)
			status, cells, err := wire.DecodeFields(reply)
			if transport != ring.ModuleStatusOK || err != nil || len(reply) > int(ring.ModuleMessageMaxBody) {
				t.Fatal("invalid envelope", transport, status, len(reply), err)
			}
			if excess == 0 {
				const index = 5
				if status != domain.StatusOK || len(cells) <= index || cells[index] != raw || len(reply) != int(ring.ModuleMessageMaxBody) {
					t.Fatal("exact-boundary UTF8 source changed", status, len(cells), len(reply))
				}
			} else if status != domain.StatusTooLong || len(cells) != 0 || len(reply) != 8 {
				t.Fatal("over-frame source did not return complete typed refusal", status, len(cells), len(reply))
			}
			var unchanged bool
			if err = pool.QueryRow(ctx, `SELECT messages_json=$1 FROM primary_sessions WHERE session_id=$2`, raw, session).Scan(&unchanged); err != nil || !unchanged {
				t.Fatal("boundary read changed source", err)
			}
		})
	}
}
