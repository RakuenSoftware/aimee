package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func sourceReleaseArgs(value map[string]any) commandArgs {
	raw, _ := json.Marshal(value)
	var args commandArgs
	_ = json.Unmarshal(raw, &args)
	return args
}
func sourceReleaseCall(t *testing.T, s *sourceReleaseState, args commandArgs) map[string]any {
	t.Helper()
	raw, status := handleSourceRelease(s, args)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	body, err := bus.DecodeCommandResult(raw)
	var result map[string]any
	if err != nil || json.Unmarshal(body, &result) != nil {
		t.Fatal(err, string(body))
	}
	return result
}
func releaseTestRef() typedProjectionRef {
	return typedProjectionRef{Channel: "facts", ID: "9007199254743001", Source: &typedSourceVersion{
		Kind: "semantic_assertion", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "9007199254743001", RecordRevision: "2"}, MemoryParentState: "observed"}}
}
func TestSourceReleaseFreshAnswerAndFailureIsolation(t *testing.T) {
	for _, scenario := range []string{"unchanged", "changed", "outage", "wrong-check", "wrong-digest", "different-principal", "restart", "expired", "replay", "superseded-attempt"} {
		t.Run(scenario, func(t *testing.T) {
			s := &sourceReleaseState{}
			args := sourceReleaseArgs(map[string]any{"request_id": "request", "principal": "owner", "project": "app"})
			refs := []typedProjectionRef{releaseTestRef()}
			assembly := map[string]any{"facts_projection": map[string]any{"retained_items": refs}}
			ticket, err := s.prepare(args, assembly)
			if err != nil || !releaseTokenValid(ticket) {
				t.Fatal(ticket, err)
			}
			args["source_release_ticket"], _ = json.Marshal(ticket)
			args["operation"] = json.RawMessage(`"source-release-plan"`)
			plan := sourceReleaseCall(t, s, args)
			request := plan["request"].(map[string]any)
			if request["scope_context"] != true || request["include_all"] != false || request["project"] != "app" {
				t.Fatal(request)
			}
			check := request["revalidation"].(map[string]any)["check_id"]
			reply := map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest(refs)}
			switch scenario {
			case "changed":
				reply["eligible"] = false
			case "outage":
				reply = map[string]any{"status": "unavailable"}
			case "wrong-check":
				reply["check_id"] = strings.Repeat("0", 32)
			case "wrong-digest":
				reply["sources_digest"] = "different"
			case "different-principal":
				args["principal"] = json.RawMessage(`"other"`)
			case "restart":
				s = &sourceReleaseState{}
			case "expired":
				s.entries[ticket].expires = time.Now().Add(-time.Second)
			case "superseded-attempt":
				sourceReleaseCall(t, s, args)
			}
			args["operation"] = json.RawMessage(`"source-release-result"`)
			args["owner_response"], _ = json.Marshal(reply)
			result := sourceReleaseCall(t, s, args)
			if scenario == "unchanged" || scenario == "replay" {
				if result["admitted"] != true {
					t.Fatal(result)
				}
				if result = sourceReleaseCall(t, s, args); result["admitted"] == true {
					t.Fatal("reused old owner answer", result)
				}
			} else if result["admitted"] == true || result["status"] != "error" {
				t.Fatal(result)
			}
		})
	}
}
func TestSourceReleaseCapacityAndAccumulation(t *testing.T) {
	s := &sourceReleaseState{}
	args := sourceReleaseArgs(map[string]any{"request_id": "request", "project": "app"})
	ref := releaseTestRef()
	assembly := map[string]any{"facts_projection": map[string]any{"retained_items": []typedProjectionRef{ref}}}
	token, err := s.prepare(args, assembly)
	if err != nil {
		t.Fatal(err)
	}
	args["source_release_ticket"], _ = json.Marshal(token)
	second, err := s.prepare(args, assembly)
	if err != nil || string(s.entries[second].sources) != string(s.entries[token].sources) {
		t.Fatal("duplicate retention", err)
	}
	ref.Source.Version.RecordRevision = "3"
	third, err := s.prepare(args, assembly)
	var refs []typedProjectionRef
	if err != nil || json.Unmarshal(s.entries[third].sources, &refs) != nil || len(refs) != 2 {
		t.Fatal("lost earlier retained source", err, refs)
	}
	args["project"] = json.RawMessage(`"other"`)
	if _, err = s.prepare(args, assembly); err == nil {
		t.Fatal("widened earlier scope")
	}
	delete(args, "source_release_ticket")
	s.bytes = sourceReleaseMaxBytes
	if _, err = s.prepare(args, assembly); err == nil {
		t.Fatal("unbounded source state")
	}
	if s.entries[token] == nil {
		t.Fatal("capacity eviction invalidated live handle")
	}
}

func TestSourceReleaseConcurrentRequests(t *testing.T) {
	s := &sourceReleaseState{}
	t.Cleanup(func() {
		if len(s.entries) != 0 || s.bytes != 0 {
			t.Fatal("completed requests retained source cache", len(s.entries), s.bytes)
		}
	})
	refs := []typedProjectionRef{releaseTestRef()}
	assembly := map[string]any{"facts_projection": map[string]any{"retained_items": refs}}
	for i := 0; i < 64; i++ {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			t.Parallel()
			args := sourceReleaseArgs(map[string]any{"request_id": fmt.Sprint(i), "principal": "same-user"})
			ticket, err := s.prepare(args, assembly)
			if err != nil {
				t.Fatal(err)
			}
			args["source_release_ticket"], _ = json.Marshal(ticket)
			args["operation"] = json.RawMessage(`"source-release-plan"`)
			plan := sourceReleaseCall(t, s, args)
			request := plan["request"].(map[string]any)["revalidation"].(map[string]any)
			args["operation"] = json.RawMessage(`"source-release-result"`)
			args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true,
				"check_id": request["check_id"], "sources_digest": releaseDigest(refs)})
			if reply := sourceReleaseCall(t, s, args); reply["admitted"] != true {
				t.Fatal(reply)
			}
			args["operation"] = json.RawMessage(`"source-release-finish"`)
			if reply := sourceReleaseCall(t, s, args); reply["status"] != "ok" {
				t.Fatal(reply)
			}
		})
	}
}

func TestSourceReleaseRejectedCandidatePreservesAcceptedSources(t *testing.T) {
	s := &sourceReleaseState{}
	args := sourceReleaseArgs(map[string]any{"request_id": "request"})
	ref := releaseTestRef()
	assembly := map[string]any{"facts_projection": map[string]any{"retained_items": []typedProjectionRef{ref}}}
	accepted, err := s.prepare(args, assembly)
	if err != nil {
		t.Fatal(err)
	}
	prior, priorBytes := string(s.entries[accepted].sources), s.bytes
	args["source_release_ticket"], _ = json.Marshal(accepted)
	ref.Source.Version.RecordRevision = "3"
	candidate, err := s.prepare(args, assembly)
	if err != nil || candidate == accepted || string(s.entries[accepted].sources) != prior {
		t.Fatal("candidate changed accepted sources", err)
	}
	args["source_release_ticket"], _ = json.Marshal(candidate)
	args["operation"] = json.RawMessage(`"source-release-discard"`)
	if result := sourceReleaseCall(t, s, args); result["status"] != "ok" {
		t.Fatal(result)
	}
	if len(s.entries) != 1 || s.bytes != priorBytes || string(s.entries[accepted].sources) != prior {
		t.Fatal("rejection damaged accepted handle")
	}
	args["source_release_ticket"], _ = json.Marshal(accepted)
	candidate, err = s.prepare(args, assembly)
	if err != nil {
		t.Fatal(err)
	}
	args["source_release_ticket"], _ = json.Marshal(candidate)
	args["operation"] = json.RawMessage(`"source-release-plan"`)
	if result := sourceReleaseCall(t, s, args); result["status"] != "ok" {
		t.Fatal(result)
	}
	if len(s.entries) != 1 || s.entries[accepted] != nil {
		t.Fatal("accepted replacement leaked old handle")
	}
	args["operation"] = json.RawMessage(`"source-release-finish"`)
	sourceReleaseCall(t, s, args)
	if len(s.entries) != 0 || s.bytes != 0 {
		t.Fatal("completion leaked source state")
	}
}

func exerciseSourceRevalidationReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore, refs []typedProjectionRef) {
	t.Helper()
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), Sources: refs}
	check := func(want bool) {
		t.Helper()
		ok, err := backend.revalidateSources(ctx, request, Scope{})
		if err != nil || ok != want {
			t.Fatal("source check", ok, want, err)
		}
	}
	check(true)
	id, parent := refs[0].ID, refs[0].Source.MemoryParents[0].RecordID
	for _, query := range []string{
		`UPDATE entity_edges SET version=version+1 WHERE id=` + id,
		`UPDATE entity_edges SET suppressed=1 WHERE id=` + id,
		`UPDATE entity_edges SET valid_until=CURRENT_TIMESTAMP::text WHERE id=` + id,
		`UPDATE memories SET content='changed at handoff' WHERE id=` + parent,
		`UPDATE memories SET activation_suppressed=1 WHERE id=` + parent,
		`UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE id=` + parent,
		`INSERT INTO fact_evidence(assertion_id,source_kind,source_id) VALUES (` + id + `,'memory','memory:9223372036854775807')`,
		`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,invalidated_at) VALUES (` + id + `,'memory','memory:9223372036854775807',CURRENT_TIMESTAMP::text)`,
		`SELECT set_config('aimee.memory_project','different-project',true)`,
	} {
		exec("SAVEPOINT release_change")
		exec(query)
		check(false)
		exec("ROLLBACK TO SAVEPOINT release_change; RELEASE SAVEPOINT release_change")
		check(true)
	}
	// A historical request retains its time policy instead of being tested as
	// a current fact. Current parent eligibility still applies.
	exec("SAVEPOINT release_history")
	exec(`UPDATE entity_edges SET valid_until='2020-01-01 00:00:00' WHERE id=` + id)
	copyRef := refs[0]
	source := *copyRef.Source
	copyRef.Source = &source
	copyRef.Channel = "historical_assertions"
	source.ReadPolicy = &sourceReadPolicy{Historical: true}
	request.Sources = []typedProjectionRef{copyRef}
	check(true)
	source.ReadPolicy = &sourceReadPolicy{ValidAt: "2019-01-01T00:00:00Z", Historical: true}
	check(true)
	source.ReadPolicy.ValidAt = "2021-01-01T00:00:00Z"
	check(false)
	exec("ROLLBACK TO SAVEPOINT release_history; RELEASE SAVEPOINT release_history")
	request.Sources = refs
	check(true)
	// Exercise the authenticated command and scoped data dispatcher, not only SQL.
	args := map[string]any{"scope_context": true, "project": "current-fact-project", "revalidation": request}
	raw, _ := json.Marshal(args)
	result, status := invokeContextCommand(t, NewHandler(nil, WithDataStore(PlacementKB, backend)), 0, bus.CommandContext{}, "revalidate_sources", string(raw))
	if status != bus.ModuleStatusOK || result["eligible"] != true || result["sources_digest"] != releaseDigest(refs) {
		t.Fatal(result, status)
	}
	// Episode roots and parents participate in the same statement as assertions.
	exec("SAVEPOINT release_episode")
	exec(`INSERT INTO memory_episodes(memory_id,episode_key,episode_text) VALUES($1,'release-episode','handoff episode')`, parent)
	episodes, err := backend.typedEpisodes(ctx, "release-episode", 2, Scope{})
	if err != nil || len(episodes) != 1 {
		t.Fatal(episodes, err)
	}
	ep := episodes[0]
	request.Sources = append(append([]typedProjectionRef{}, refs...), typedProjectionRef{Channel: "episodes", ID: fmt.Sprint(ep.ID), Source: ep.source})
	check(true)
	exec(`UPDATE memory_episodes SET episode_text='new episode' WHERE id=$1`, ep.ID)
	check(false)
	exec("ROLLBACK TO SAVEPOINT release_episode; RELEASE SAVEPOINT release_episode")
}
