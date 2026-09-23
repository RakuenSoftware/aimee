package memory

import (
	"context"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestNegationTokensAndBoost(t *testing.T) {
	for _, tc := range []struct {
		text string
		want []string
	}{
		{"I do not cache. Disk is persistent.", []string{"not_cache"}},
		{"Redis runs without persistence.", []string{"not_redis", "not_runs", "not_persistence"}},
		{"No: cache persists.", []string{}},
		{"not x. cache", []string{}},
		{"We can't retain credentials", []string{"not_retain", "not_credentials"}},
	} {
		if got := negationTokens(tc.text); !reflect.DeepEqual(got, tc.want) {
			t.Fatal(tc.text, got, tc.want)
		}
	}
	if tokens := negationTokens(strings.Repeat("never cache credentials ", 300)); len(strings.Join(tokens, " ")) >= 2048 {
		t.Fatal("unbounded negation tokens")
	}
	query := []string{"not_cache", "not_credentials"}
	if got := negationOverlap(query, query); got != 6 {
		t.Fatal(got)
	}
	if got := negationOverlap(query, []string{"not_cache"}); got != 2.25 {
		t.Fatal(got)
	}
	if got := negationOverlap(nil, query); got != 0 {
		t.Fatal(got)
	}
}

func exerciseNegationReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	seed := func(content, scope, updated string, confidence float64) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,updated_at,confidence)
 VALUES('L2','fact','sentinelRedis persistence',$1,'project',$2,$3,$4) RETURNING id`, content, scope, updated, confidence).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	negative := seed("sentinelRedis runs without persistence.", "neg-visible", "2020-01-01", .1)
	positive := seed("sentinelRedis uses persistence.", "neg-visible", "2026-09-18", .99)
	hidden := seed("sentinelRedis runs without persistence.", "neg-hidden", "2027-01-01", 1)
	var extra, retired, shared int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','alternate deployment','deployment never writes disk','project','neg-visible') RETURNING id`).Scan(&extra); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,negation_tokens)
 VALUES('L2','fact','archived deployment','retired','project','neg-visible','archived','not_disk') RETURNING id`).Scan(&retired); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','shared deployment','deployment never writes disk','workspace','_shared') RETURNING id`).Scan(&shared); err != nil {
		t.Fatal(err)
	}
	originalSettings := backend.settings
	defer func() { backend.settings = originalSettings }()
	enabled := true
	backend.settings = func() (map[string]any, error) { return map[string]any{"memory_negation_enabled": enabled}, nil }
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	if r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", `{"scope_context":true,"project":"neg-visible"}`); status != bus.ModuleStatusOK || r["status"] != "ok" {
		t.Fatal(r, status)
	}
	search := func(query string) []int64 {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "find_facts_visible", fmt.Sprintf(`{"query":%q,"project":"neg-visible","limit":8}`, query))
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(r, status)
		}
		ids := []int64{}
		for _, row := range r["facts"].([]any) {
			ids = append(ids, int64(row.(map[string]any)["id"].(float64)))
		}
		return ids
	}
	enabled = false
	before := search("sentinelRedis not persistence")
	if len(before) != 2 || before[0] != positive {
		t.Fatal("baseline no longer discriminates polarity", before, positive)
	}
	control := search("sentinelRedis persistence")
	enabled = true
	after := search("sentinelRedis not persistence")
	if len(after) != 2 || after[0] != negative {
		t.Fatal("negation failed to outrank affirmative twin", after, negative)
	}
	if got := search("sentinelRedis persistence"); !reflect.DeepEqual(got, control) {
		t.Fatal("positive query order changed", control, got)
	}
	ids := search("sentinelRedis not disk")
	found, foundShared := false, false
	for _, id := range ids {
		found = found || id == extra
		foundShared = foundShared || id == shared
		if id == hidden || id == retired {
			t.Fatal("negation lane leaked ineligible record", ids)
		}
	}
	if !found || !foundShared {
		t.Fatal("negation index failed to widen recall", ids)
	}
	// An exact project request still excludes shared rows even with IncludeAll.
	exact, err := backend.finalizeRecall(ctx, DataRequest{Query: "sentinelRedis not disk", Scope: Scope{Type: ScopeProject, Value: "neg-visible"}, Project: "neg-visible", IncludeAll: true, Limit: 8}, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(exact) == 0 {
		t.Fatal("exact negation recall lost local result")
	}
	for _, row := range exact {
		if row.Scope.Type != ScopeProject || row.Scope.Value != "neg-visible" {
			t.Fatal("exact negation scope widened", exact)
		}
	}
	// The optional lane must gate validity before its own 64-row cap. A
	// post-filter would let newer invalid rows starve the older eligible match.
	forbidden := map[int64]bool{hidden: true, retired: true}
	for i := 0; i < 70; i++ {
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,negation_tokens,valid_until)
 VALUES('L2','fact',$1,'deployment never writes disk','project','neg-visible','not_disk','2000-01-01') RETURNING id`,
			fmt.Sprintf("expired negation %d", i)).Scan(&id); err != nil {
			t.Fatal(err)
		}
		forbidden[id] = true
	}
	for _, clause := range []string{
		"valid_from='2999-01-01'", "valid_until=CURRENT_TIMESTAMP::text",
		"activation_suppressed=1", "lifecycle_state='superseded'",
		"lifecycle_state='quarantined'", "lifecycle_state='deleted'",
		"lifecycle_state='revoked'", "lifecycle_state='rejected'",
	} {
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,negation_tokens)
 VALUES('L2','fact',$1,'deployment never writes disk','project','neg-visible','not_disk') RETURNING id`, clause).Scan(&id); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, "UPDATE memories SET "+clause+" WHERE id=$1", id); err != nil {
			t.Fatal(err)
		}
		forbidden[id] = true
	}
	ids = search("sentinelRedis not disk")
	found, foundShared = false, false
	for _, id := range ids {
		if forbidden[id] {
			t.Fatal("negation validity leak", id, ids)
		}
		found = found || id == extra
		foundShared = foundShared || id == shared
	}
	if !found || !foundShared {
		t.Fatal("ineligible negation candidates consumed lane cap", ids)
	}
	exact, err = backend.finalizeRecall(ctx, DataRequest{Query: "sentinelRedis not disk", Scope: Scope{Type: ScopeProject, Value: "neg-visible"}, Project: "neg-visible", Limit: 8}, true, nil)
	if err != nil || len(exact) == 0 {
		t.Fatal(exact, err)
	}
	for _, row := range exact {
		if forbidden[row.ID] || row.ID == shared {
			t.Fatal("exact negation eligibility leak", row)
		}
	}
	// Both normalized endpoint spellings use the request's stable database
	// clock. Malformed nonempty endpoints fail closed instead of reopening.
	if _, err := tx.Exec(ctx, "UPDATE memories SET valid_from=CURRENT_TIMESTAMP::text,valid_until=(CURRENT_TIMESTAMP+interval '1 hour')::text WHERE id=$1", extra); err != nil {
		t.Fatal(err)
	}
	found = false
	for _, id := range search("sentinelRedis not disk") {
		found = found || id == extra
	}
	if !found {
		t.Fatal("valid lower endpoint excluded")
	}
	if _, err := tx.Exec(ctx, "UPDATE memories SET valid_until='tomorrow' WHERE id=$1", extra); err != nil {
		t.Fatal(err)
	}
	malformed, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "find_facts_visible", `{"query":"sentinelRedis not disk","project":"neg-visible","limit":8}`)
	if status != bus.ModuleStatusOK || malformed["kind"] != "unavailable" {
		t.Fatal("malformed validity accepted", malformed, status)
	}
	if _, err := tx.Exec(ctx, "UPDATE memories SET valid_from='',valid_until='' WHERE id=$1", extra); err != nil {
		t.Fatal(err)
	}
	enabled = false
	for _, id := range search("sentinelRedis not disk") {
		if id == extra {
			t.Fatal("disabled negation lane still executed")
		}
	}
}
