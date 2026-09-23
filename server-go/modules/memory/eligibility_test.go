package memory

import (
	"context"
	"encoding/json"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

func exerciseCurrentEligibilityReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT current_eligibility_replay`)
	defer exec(`ROLLBACK TO SAVEPOINT current_eligibility_replay; RELEASE SAVEPOINT current_eligibility_replay`)
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true); SET LOCAL TIME ZONE 'Asia/Tokyo'`)
	ids := map[string]int64{}
	for _, key := range []string{"open", "utc-boundary", "offset-boundary", "expired", "future", "suppressed", "superseded", "archived", "retired", "quarantined", "deleted", "revoked", "rejected", "unknown", "private"} {
		scope, life, suppressed := "eligibility-local", "active", 0
		switch key {
		case "private":
			scope = "eligibility-private"
		case "suppressed":
			suppressed = 1
		case "superseded", "archived", "retired":
			life, suppressed = key, 1
		case "quarantined", "deleted", "revoked", "rejected", "unknown":
			life = key
		}
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(key,content,tier,kind,scope_type,scope_value,lifecycle_state,activation_suppressed)
VALUES($1,'eligibilityneedle','L2','fact','project',$2,$3,$4) RETURNING id`, "eligibility-"+key, scope, life, suppressed).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids[key] = id
	}
	exec(`UPDATE memories SET valid_from=to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS.US') WHERE key='eligibility-utc-boundary';
UPDATE memories SET valid_from='  '||CURRENT_TIMESTAMP::text||'  ' WHERE key='eligibility-offset-boundary';
UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE key='eligibility-expired';
UPDATE memories SET valid_from=(CURRENT_TIMESTAMP+interval '1 second')::text WHERE key='eligibility-future';
SET LOCAL ROLE aimee_store_runtime`)
	bound := *backend
	bound.fusionEnabled = false
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, &bound)))
	check := func(verb string, args map[string]any) {
		t.Helper()
		raw, _ := json.Marshal(args)
		r := runPublicCommand(t, client, verb, string(raw))
		if r["status"] != "ok" {
			t.Fatal(verb, r)
		}
		field := "facts"
		if verb == "list" {
			field = "memories"
		}
		got := []string{}
		for _, item := range r[field].([]any) {
			key := item.(map[string]any)["key"].(string)
			if strings.HasPrefix(key, "eligibility-") {
				got = append(got, key)
			}
		}
		sort.Strings(got)
		if strings.Join(got, ",") != "eligibility-offset-boundary,eligibility-open,eligibility-utc-boundary" {
			t.Fatal(verb, got)
		}
		if verb == "search" && len(r["windows"].([]any)) != 3 {
			t.Fatal("compatibility windows bypassed validity", r["windows"])
		}
	}
	for _, verb := range []string{"find_facts", "find_facts_visible", "find_facts_scoped", "list", "search"} {
		args := map[string]any{"query": "eligibilityneedle", "project": "eligibility-local", "scope_context": true, "limit": 64}
		if verb == "find_facts_scoped" {
			args["scope_type"], args["scope_value"] = "project", "eligibility-local"
		}
		if verb == "search" {
			args["view"], args["keywords"], args["limit"] = "server", []string{"eligibilityneedle"}, 32
		}
		check(verb, args)
	}
	// Legacy session/query modes must apply the same gate before their limit.
	// Ineligible rows have equal priority and later IDs; filtering after LIMIT
	// would starve the three eligible fixtures rather than returning them.
	exec(`UPDATE memories SET confidence=1,use_count=1000000 WHERE key LIKE 'eligibility-%'`)
	for _, mode := range []string{"like", "top-l2", "session-priority", "facts-patterns", "eval"} {
		records, err := bound.QueryRecords(ctx, mode, "%eligibilityneedle%", 0, 3)
		if err != nil {
			t.Fatal(mode, err)
		}
		got := []string{}
		for _, record := range records {
			got = append(got, record.Key)
		}
		sort.Strings(got)
		if strings.Join(got, ",") != "eligibility-offset-boundary,eligibility-open,eligibility-utc-boundary" {
			t.Fatal("query mode current eligibility", mode, got)
		}
	}
	// History is a retained-version view, not an escape hatch for erased or
	// unauthorized content. Exercise explicit scope on the public command.
	for key := range ids {
		allowed := key == "open" || key == "utc-boundary" || key == "offset-boundary" || key == "expired" || key == "future" || key == "superseded" || key == "archived" || key == "retired"
		args, _ := json.Marshal(map[string]any{"key": "eligibility-" + key, "max": 1, "scope_context": true, "project": "eligibility-local"})
		result := runPublicCommand(t, client, "fact_history", string(args))
		if result["status"] != "ok" {
			t.Fatal("history eligibility", key, result)
		}
		rows, ok := result["history"].([]any)
		if !ok || (len(rows) == 1) != allowed || len(rows) > 1 {
			t.Fatal("history eligibility or scope", key, result)
		}
	}
	exec(`SAVEPOINT historical_limit; UPDATE memories SET key='historical-eligibility#v'||id::text WHERE key LIKE 'eligibility-%'`)
	args, _ := json.Marshal(map[string]any{"key": "historical-eligibility", "max": 3, "scope_context": true, "project": "eligibility-local"})
	result := runPublicCommand(t, client, "fact_history", string(args))
	if result["status"] != "ok" {
		t.Fatal(result)
	}
	rows, ok := result["history"].([]any)
	if !ok || len(rows) != 3 {
		t.Fatal("excluded history crowded out retained versions", result)
	}
	expected := map[int64]bool{ids["retired"]: true, ids["archived"]: true, ids["superseded"]: true}
	for _, row := range rows {
		if !expected[int64(row.(map[string]any)["id"].(float64))] {
			t.Fatal("history limit preceded eligibility", result)
		}
	}
	exec(`ROLLBACK TO SAVEPOINT historical_limit; RELEASE SAVEPOINT historical_limit`)
	// An exact ID must not bypass the same current-state gates as a search.
	// Legacy as_of is a labeled inspection of an old version, but never grants
	// access to erased, revoked, quarantined, rejected or cross-scope content.
	for key, id := range ids {
		for _, historical := range []bool{false, true} {
			args := map[string]any{"id": id, "scope_context": true, "project": "eligibility-local"}
			allowed := key == "open" || key == "utc-boundary" || key == "offset-boundary"
			if historical {
				args["as_of"] = "2026-01-01"
				allowed = allowed || key == "expired" || key == "future" || key == "superseded" || key == "archived" || key == "retired"
			}
			raw, _ := json.Marshal(args)
			got := runPublicCommand(t, client, "get", string(raw))
			if allowed {
				if got["status"] != "ok" || got["memory"] == nil {
					t.Fatalf("get %s historical=%v withheld an eligible row: %v", key, historical, got)
				}
			} else if got["kind"] != "not_found" || got["memory"] != nil {
				t.Fatalf("get %s historical=%v released excluded content: %v", key, historical, got)
			}
		}
	}
	exerciseReadPolicyReplay(t, ctx, tx, client, ids)
	// Serving suppression must not make an otherwise admitted retirement
	// impossible. The mutation path owns its own author and scope checks.
	if changed, err := bound.Delete(ctx, Scope{Type: ScopeProject, Value: "eligibility-local"}, ids["suppressed"]); err != nil || !changed {
		t.Fatalf("retiring suppressed memory: changed=%v err=%v", changed, err)
	}
	// The half-open endpoints agree with the storage owner's canonical timestamp
	// adapter even when the PostgreSQL session is not in UTC.
	var same bool
	if err := tx.QueryRow(ctx, `SELECT bool_and((`+memoryTimeSQL("valid_from")+`) IS NOT DISTINCT FROM aimee_utc_text_timestamptz(btrim(valid_from))) FROM memories WHERE key LIKE 'eligibility-%'`).Scan(&same); err != nil || !same {
		t.Fatal("timestamp normalization diverged", same, err)
	}
	for _, malformed := range []string{"not-a-timestamp", "now", "tomorrow", "infinity", "-infinity", "2026-02-30"} {
		exec(`UPDATE memories SET valid_from=$1 WHERE key='eligibility-open'`, malformed)
		args, _ := json.Marshal(map[string]any{"id": ids["open"], "scope_context": true, "project": "eligibility-local"})
		if r := runPublicCommand(t, client, "get", string(args)); r["kind"] != "unavailable" || r["memory"] != nil {
			t.Fatal("direct read admitted malformed governed time", malformed, r)
		}
		if r := runPublicCommand(t, client, "find_facts", `{"query":"eligibilityneedle","scope_context":true,"project":"eligibility-local"}`); r["kind"] != "unavailable" || r["facts"] != nil {
			t.Fatal("malformed governed time admitted", malformed, r)
		}
	}
	exec(`UPDATE memories SET valid_from=NULL WHERE key='eligibility-open'`)
	check("search", map[string]any{"view": "server", "keywords": []string{"eligibilityneedle"}, "project": "eligibility-local", "scope_context": true, "limit": 32})
}

func exerciseReadPolicyReplay(t *testing.T, ctx context.Context, tx pgx.Tx, client *Client, ids map[string]int64) {
	t.Helper()
	var now time.Time
	if err := tx.QueryRow(ctx, `SELECT CURRENT_TIMESTAMP`).Scan(&now); err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"current", "historical"} {
		for key, id := range ids {
			policy := MemoryReadPolicy{SchemaVersion: 1, Mode: mode}
			allowed := key == "open" || key == "utc-boundary" || key == "offset-boundary"
			if mode == "historical" {
				// The session timezone is Tokyo; send another explicit offset.
				policy.ValidAt = now.In(time.FixedZone("fixture", -7*60*60)).Format(time.RFC3339Nano)
				allowed = allowed || key == "superseded" || key == "archived" || key == "retired"
			}
			raw, _ := json.Marshal(map[string]any{"id": id, "scope_context": true, "project": "eligibility-local", "read_policy": policy})
			got := runPublicCommand(t, client, "get", string(raw))
			if !allowed {
				if got["kind"] != "not_found" || got["memory"] != nil || got["read"] != nil {
					t.Fatalf("versioned %s leaked %s: %v", mode, key, got)
				}
				continue
			}
			if got["status"] != "ok" || got["memory"] == nil || got["read"] == nil {
				t.Fatalf("versioned %s excluded %s: %v", mode, key, got)
			}
			decision := got["read"].(map[string]any)
			if decision["mode"] != mode || decision["schema_version"] != float64(1) || decision["policy_version"] != currentEligibilityPolicy {
				t.Fatal("wrong applied policy", decision)
			}
			if mode == "historical" && decision["valid_at"] != now.UTC().Format(time.RFC3339Nano) {
				t.Fatal("historical clock not normalized", decision)
			}
		}
	}
	for _, tc := range []struct {
		key     string
		offset  time.Duration
		allowed bool
	}{
		{"expired", -time.Microsecond, true}, {"expired", 0, false},
		{"utc-boundary", -time.Microsecond, false}, {"utc-boundary", 0, true},
		{"future", time.Second - time.Microsecond, false}, {"future", time.Second, true},
		{"revoked", -time.Hour, false}, {"quarantined", -time.Hour, false}, {"private", -time.Hour, false},
	} {
		raw, _ := json.Marshal(map[string]any{"id": ids[tc.key], "scope_context": true, "project": "eligibility-local", "read_policy": MemoryReadPolicy{
			SchemaVersion: 1, Mode: "historical", ValidAt: now.Add(tc.offset).UTC().Format(time.RFC3339Nano),
		}})
		got := runPublicCommand(t, client, "get", string(raw))
		if (got["status"] == "ok") != tc.allowed || (!tc.allowed && (got["kind"] != "not_found" || got["memory"] != nil)) {
			t.Fatalf("historical boundary %s offset=%s: %v", tc.key, tc.offset, got)
		}
	}
}
