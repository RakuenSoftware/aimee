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
	var extra, retired int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','alternate deployment','deployment never writes disk','project','neg-visible') RETURNING id`).Scan(&extra); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,negation_tokens)
 VALUES('L2','fact','archived deployment','retired','project','neg-visible','archived','not_disk') RETURNING id`).Scan(&retired); err != nil {
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
	found := false
	for _, id := range ids {
		found = found || id == extra
		if id == hidden || id == retired {
			t.Fatal("negation lane leaked ineligible record", ids)
		}
	}
	if !found {
		t.Fatal("negation index failed to widen recall", ids)
	}
	enabled = false
	for _, id := range search("sentinelRedis not disk") {
		if id == extra {
			t.Fatal("disabled negation lane still executed")
		}
	}
}
