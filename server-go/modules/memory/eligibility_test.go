package memory

import (
	"context"
	"encoding/json"
	"sort"
	"strings"
	"testing"

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
	for _, key := range []string{"open", "utc-boundary", "offset-boundary", "expired", "future", "suppressed", "superseded", "archived", "quarantined", "deleted", "revoked", "private"} {
		scope, life, suppressed := "eligibility-local", "active", 0
		switch key {
		case "private":
			scope = "eligibility-private"
		case "suppressed":
			suppressed = 1
		case "superseded", "archived", "quarantined", "deleted", "revoked":
			life = key
		}
		exec(`INSERT INTO memories(key,content,tier,kind,scope_type,scope_value,lifecycle_state,activation_suppressed)
VALUES($1,'eligibilityneedle','L2','fact','project',$2,$3,$4)`, "eligibility-"+key, scope, life, suppressed)
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
	// The half-open endpoints agree with the storage owner's canonical timestamp
	// adapter even when the PostgreSQL session is not in UTC.
	var same bool
	if err := tx.QueryRow(ctx, `SELECT bool_and((`+memoryTimeSQL("valid_from")+`) IS NOT DISTINCT FROM aimee_utc_text_timestamptz(btrim(valid_from))) FROM memories WHERE key LIKE 'eligibility-%'`).Scan(&same); err != nil || !same {
		t.Fatal("timestamp normalization diverged", same, err)
	}
	for _, malformed := range []string{"not-a-timestamp", "now", "tomorrow", "infinity", "-infinity", "2026-02-30"} {
		exec(`UPDATE memories SET valid_from=$1 WHERE key='eligibility-open'`, malformed)
		if r := runPublicCommand(t, client, "find_facts", `{"query":"eligibilityneedle","scope_context":true,"project":"eligibility-local"}`); r["kind"] != "unavailable" || r["facts"] != nil {
			t.Fatal("malformed governed time admitted", malformed, r)
		}
	}
	exec(`UPDATE memories SET valid_from=NULL WHERE key='eligibility-open'`)
	check("search", map[string]any{"view": "server", "keywords": []string{"eligibilityneedle"}, "project": "eligibility-local", "scope_context": true, "limit": 32})
}
