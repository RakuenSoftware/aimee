package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
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
	parentVersion := refs[0].Source.MemoryParents[0]
	native := typedProjectionRef{Channel: "native_active_context", ID: parentVersion.RecordID, Source: &typedSourceVersion{Kind: "memory_record", Version: parentVersion, MemoryParentState: "observed"}}
	request.Sources = []typedProjectionRef{native}
	check(true)
	request.Sources[0].Channel = "native_open_commitments"
	check(false) // an active record cannot substitute for a pending commitment
	request.Sources = refs
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

func TestNativeMixedOwnerReleaseRequiresBothAnswers(t *testing.T) {
	for _, scenario := range []string{"both", "private-only", "missing-local", "missing-shared", "swapped", "private-stale", "shared-stale", "old-answer"} {
		t.Run(scenario, func(t *testing.T) {
			state := &sourceReleaseState{}
			args := sourceReleaseArgs(map[string]any{"request_id": "native-request", "principal": "native-user", "project": "app"})
			private := releaseTestRef()
			private.Channel = "native_preferences"
			private.Source.Kind = "user_memory_record"
			shared := releaseTestRef()
			shared.Channel = "native_identity"
			shared.Source.Kind = "memory_record"
			shared.Source.Version.OwnerID = "00000000-0000-4000-8000-000000000002"
			p := nativeProjectionForText("accepted native context", 1000)
			collection := private
			collectionSource := *private.Source
			collection.Source = &collectionSource
			collection.Channel, collection.ID = "native_memory_collection", "1"
			collection.Source.Kind, collection.Source.Version.RecordID = "user_memory_collection", "1"
			p.Sources = []typedProjectionRef{private, collection}
			if scenario != "private-only" {
				p.Sources = append(p.Sources, shared)
			}
			p.SelectionDigest = releaseDigest(p.Sources)
			args["native_projection"], _ = json.Marshal(p)
			encoded, status := handleNativeSourceRelease(state, args)
			raw, err := bus.DecodeCommandResult(encoded)
			var accepted map[string]any
			if status != bus.ModuleStatusOK || err != nil || json.Unmarshal(raw, &accepted) != nil || accepted["status"] != "ok" {
				t.Fatal(string(raw), status, err)
			}
			args["source_release_ticket"], _ = json.Marshal(accepted["source_release_ticket"])
			args["operation"] = json.RawMessage(`"source-release-plan"`)
			plan := sourceReleaseCall(t, state, args)
			local := plan["local_request"].(map[string]any)
			localRaw, _ := json.Marshal(local["revalidation"])
			var localCheck sourceRevalidation
			if json.Unmarshal(localRaw, &localCheck) != nil || len(localCheck.Sources) != 2 || localCheck.Sources[0].Source.Kind != "user_memory_record" || localCheck.Sources[1].Source.Kind != "user_memory_collection" {
				t.Fatal("private routing", string(localRaw))
			}
			answer := func(check sourceRevalidation) map[string]any {
				return map[string]any{"status": "ok", "eligible": true, "check_id": check.CheckID, "sources_digest": releaseDigest(check.Sources)}
			}
			localReply := answer(localCheck)
			sharedReply := map[string]any{}
			if scenario == "private-only" {
				if _, present := plan["request"]; present {
					t.Fatal("private references sent to KB", plan)
				}
			} else {
				remote := plan["request"].(map[string]any)
				remoteRaw, _ := json.Marshal(remote["revalidation"])
				var sharedCheck sourceRevalidation
				if json.Unmarshal(remoteRaw, &sharedCheck) != nil || len(sharedCheck.Sources) != 1 || sharedCheck.Sources[0].Source.Kind != "memory_record" || sharedCheck.CheckID != localCheck.CheckID {
					t.Fatal("mixed routing", string(remoteRaw))
				}
				sharedReply = answer(sharedCheck)
			}
			switch scenario {
			case "missing-local":
				localReply = nil
			case "missing-shared":
				sharedReply = nil
			case "swapped":
				localReply, sharedReply = sharedReply, localReply
			case "private-stale":
				localReply["eligible"] = false
			case "shared-stale":
				sharedReply["eligible"] = false
			case "old-answer":
				sourceReleaseCall(t, state, args)
			}
			args["operation"] = json.RawMessage(`"source-release-result"`)
			args["local_response"], _ = json.Marshal(localReply)
			args["owner_response"], _ = json.Marshal(sharedReply)
			result := sourceReleaseCall(t, state, args)
			want := scenario == "both" || scenario == "private-only"
			if (result["admitted"] == true) != want {
				t.Fatal(scenario, result)
			}
			if sourceReleaseCall(t, state, args)["admitted"] == true {
				t.Fatal("owner answers reused")
			}
			p.SelectionDigest = "changed"
			args["native_projection"], _ = json.Marshal(p)
			encoded, _ = handleNativeSourceRelease(state, args)
			raw, _ = bus.DecodeCommandResult(encoded)
			if strings.Contains(string(raw), `"status":"ok"`) {
				t.Fatal("changed selection accepted", string(raw))
			}
		})
	}
}

func TestStructuredRecallSourceObservationsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL for structured source observations")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
 CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
 GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO aimee_store_runtime;
 GRANT SELECT,INSERT,UPDATE,DELETE ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true),set_config('jit','off',true)`)
	const key = "structured-source-observation"
	var parent, directive, reminder int64
	if err = tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact',$1,'parent','project',$1) RETURNING id`, key).Scan(&parent); err != nil {
		t.Fatal(err)
	}
	if err = tx.QueryRow(ctx, `INSERT INTO epistemic_directives(question,topic,cause,memory_a_id)
 VALUES($1,$1,'user_follow_up',$2) RETURNING id`, key, parent).Scan(&directive); err != nil {
		t.Fatal(err)
	}
	if err = tx.QueryRow(ctx, `INSERT INTO prospective_memories(trigger_text,action_text,recurrence)
 VALUES($1,$1,'once') RETURNING id`, key).Scan(&reminder); err != nil {
		t.Fatal(err)
	}
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true)`, key)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	ds, err := backend.directiveMatch(ctx, key, "", "", 8, true)
	if err != nil || len(ds) != 1 {
		t.Fatalf("directives %v %v", ds, err)
	}
	rs, err := backend.prospectiveMatch(ctx, key, "", "", 8, true)
	if err != nil || len(rs) != 1 {
		t.Fatalf("reminders %v %v", rs, err)
	}
	bundle := recallBundle{Directives: []recallDirective{{Directive: ds[0], Text: ds[0].Question}}, Reminders: []recallReminder{{Prospective: rs[0], Text: rs[0].ActionText, MemoryID: rs[0].ID}}}
	projection, _, err := projectNativeRecall(bundle, 8192)
	if err != nil || len(projection.Sources) != 2 {
		t.Fatalf("native observations %+v %v", projection, err)
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("b", 32), Sources: projection.Sources}
	check := func(want bool) {
		t.Helper()
		got, err := backend.revalidateSources(ctx, request, Scope{})
		if err != nil || got != want {
			t.Fatalf("revalidation got=%v want=%v err=%v", got, want, err)
		}
	}
	check(true)
	// Observation metadata survives the same JSON hop used by native assembly.
	wire, _ := json.Marshal(projection.Sources)
	var copied []typedProjectionRef
	if err = json.Unmarshal(wire, &copied); err != nil {
		t.Fatal(err)
	}
	request.Sources = copied
	check(true)
	if strings.Contains(string(wire), `"version":{"schema_version":0`) {
		t.Fatal("invented ordinal version", string(wire))
	}
	exec(`UPDATE epistemic_directives SET surfaced_count=surfaced_count+1,last_surfaced_at=now()::text,updated_at=now()::text WHERE id=$1`, directive)
	exec(`UPDATE prospective_memories SET trigger_count=trigger_count+1,last_triggered_at=now()::text,updated_at=now()::text WHERE id=$1`, reminder)
	check(true)
	// Consuming this already-selected one-shot action is an explicit retained
	// read policy, not permission to select a triggered reminder on a new recall.
	exec(`SAVEPOINT consumed_reminder`)
	if _, err := backend.ProspectiveMarkTriggered(ctx, reminder); err != nil {
		t.Fatal(err)
	}
	check(true)
	if rows, err := backend.prospectiveMatch(ctx, key, "", "", 8, true); err != nil || len(rows) != 0 {
		t.Fatal("consumed reminder reselected", rows, err)
	}
	exec(`ROLLBACK TO SAVEPOINT consumed_reminder; RELEASE SAVEPOINT consumed_reminder`)
	for _, q := range []string{
		fmt.Sprintf(`UPDATE epistemic_directives SET question='edited after selection' WHERE id=%d`, directive),
		fmt.Sprintf(`UPDATE epistemic_directives SET question='temporary edit' WHERE id=%d; UPDATE epistemic_directives SET question='%s' WHERE id=%d`, directive, key, directive),
		fmt.Sprintf(`UPDATE epistemic_directives SET topic='changed topic' WHERE id=%d`, directive),
		fmt.Sprintf(`UPDATE epistemic_directives SET state='suppressed' WHERE id=%d`, directive),
		fmt.Sprintf(`UPDATE epistemic_directives SET valid_until=now()::text WHERE id=%d`, directive),
		fmt.Sprintf(`UPDATE epistemic_directives SET memory_b_id=9223372036854775807 WHERE id=%d`, directive),
		fmt.Sprintf(`UPDATE prospective_memories SET action_text='edited action' WHERE id=%d`, reminder),
		fmt.Sprintf(`UPDATE prospective_memories SET state='triggered' WHERE id=%d`, reminder),
		fmt.Sprintf(`UPDATE prospective_memories SET valid_until=now()::text WHERE id=%d`, reminder),
		fmt.Sprintf(`UPDATE memories SET content='parent edited' WHERE id=%d`, parent),
		fmt.Sprintf(`UPDATE memories SET lifecycle_state='revoked' WHERE id=%d`, parent),
		fmt.Sprintf(`UPDATE memories SET scope_value='foreign' WHERE id=%d`, parent),
	} {
		exec(`SAVEPOINT structured_change; RESET ROLE`)
		exec(q)
		exec(`SET LOCAL ROLE aimee_store_runtime`)
		check(false)
		exec(`ROLLBACK TO SAVEPOINT structured_change; RELEASE SAVEPOINT structured_change`)
		check(true)
	}
	exec("SET LOCAL ROLE aimee_store_runtime; SAVEPOINT guarded_observation")
	request.SendGuard = "acquire"
	if eligible, e := backend.guardedSourceRevalidation(ctx, request, Scope{}); e != nil || !eligible {
		t.Fatal("guarded sources", eligible, e)
	}
	exec("UPDATE epistemic_directives SET surfaced_count=surfaced_count+1 WHERE id=$1", directive)
	if _, e := backend.ProspectiveMarkTriggered(ctx, reminder); e != nil {
		t.Fatal("guard blocked retained reminder acknowledgement", e)
	}
	check(true)
	exec("SAVEPOINT refused_write; RESET ROLE")
	if _, e := tx.Exec(ctx, "UPDATE prospective_memories SET action_text='raced' WHERE id=$1", reminder); e == nil || !strings.Contains(e.Error(), "55P03") {
		t.Fatal("guarded edit admitted", e)
	}
	exec("ROLLBACK TO SAVEPOINT refused_write; RELEASE SAVEPOINT refused_write")
	request.SendGuard = "release"
	if _, e := backend.guardedSourceRevalidation(ctx, request, Scope{}); e != nil {
		t.Fatal(e)
	}
	exec("ROLLBACK TO SAVEPOINT guarded_observation; RELEASE SAVEPOINT guarded_observation; RESET ROLE")
	// A currently valid source that expires during the send window is refused.
	exec("SAVEPOINT expiring_guard")
	exec("UPDATE prospective_memories SET valid_until=(clock_timestamp()+interval '2 seconds')::text WHERE id=$1", reminder)
	exec("SET LOCAL ROLE aimee_store_runtime")
	fresh, e := backend.prospectiveMatch(ctx, key, "", "", 8, true)
	if e != nil || len(fresh) != 1 {
		t.Fatal(fresh, e)
	}
	expiryRequest := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("d", 32), Sources: []typedProjectionRef{{Channel: "native_reminders", ID: fmt.Sprint(reminder), Source: fresh[0].Source}}}
	if eligible, e := backend.revalidateSources(ctx, expiryRequest, Scope{}); e != nil || !eligible {
		t.Fatal("current expiry fixture", eligible, e)
	}
	expiryRequest.SendGuard = "acquire"
	if eligible, e := backend.guardedSourceRevalidation(ctx, expiryRequest, Scope{}); e != nil || eligible {
		t.Fatal("expiring source admitted", eligible, e)
	}
	exec("ROLLBACK TO SAVEPOINT expiring_guard; RELEASE SAVEPOINT expiring_guard")
}

// The shipping storage barrier is tested independently of the HTTP guard. A
// passing result here is not evidence that provider call sites hold a lease.
func TestSourceSendStorageBarrierPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("packaged PostgreSQL required")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	admin, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close(context.Background())
	other, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close(context.Background())
	schema := fmt.Sprintf("send_guard_%d", time.Now().UnixNano())
	quoted := pgx.Identifier{schema}.Sanitize()
	role := schema + "_runtime"
	qrole := pgx.Identifier{role}.Sanitize()
	exec := func(c *pgx.Conn, sql string, args ...any) {
		t.Helper()
		if _, e := c.Exec(ctx, sql, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(admin, "CREATE SCHEMA "+quoted)
	exec(admin, "CREATE ROLE "+qrole+" NOLOGIN NOSUPERUSER NOBYPASSRLS")
	defer func() {
		_, _ = admin.Exec(context.Background(), "ROLLBACK; RESET ROLE; DROP SCHEMA "+quoted+" CASCADE; DROP ROLE "+qrole)
	}()
	exec(admin, "SET search_path="+quoted+",public")
	exec(other, "SET search_path="+quoted+",public")
	names := []string{"memory_links", "memory_scopes", "learning_observations", "learning_proposals", "memory_relations", "rules", "memories", "memory_collection_owner", "memory_units", "memory_lineage", "memory_episodes", "memory_summaries", "derived_memory_dependencies", "entity_edges", "fact_evidence", "epistemic_directives", "prospective_memories"}
	for _, name := range names {
		exec(admin, "CREATE TABLE "+name+"(id integer PRIMARY KEY,value integer NOT NULL DEFAULT 0,use_count integer NOT NULL DEFAULT 0)")
	}
	raw, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	source := string(raw)
	a := strings.Index(source, "-- BEGIN memory send guards")
	b := strings.Index(source, "-- END memory send guards")
	if a < 0 || b < a {
		t.Fatal("shipping send barrier missing")
	}
	ddl := source[a:b]
	exec(admin, ddl)
	exec(admin, ddl)
	exec(admin, "INSERT INTO memories(id) VALUES(1),(2)")
	exec(admin, "GRANT USAGE ON SCHEMA "+quoted+" TO "+qrole)
	exec(admin, "GRANT SELECT,UPDATE ON memories TO "+qrole)
	exec(admin, "GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO "+qrole)
	exec(admin, "SET ROLE "+qrole)
	exec(other, "SET ROLE "+qrole)
	denied := func(sql string) {
		t.Helper()
		if _, e := other.Exec(ctx, sql); e == nil {
			t.Fatalf("unexpected admission: %s", sql)
		}
	}
	denied("SELECT token FROM memory_send_leases")
	denied("UPDATE memory_send_barrier SET blocked_until='-infinity'")
	token := strings.Repeat("a", 32)
	second := strings.Repeat("b", 32)
	exec(admin, "BEGIN")
	exec(admin, "UPDATE memories SET value=value+1 WHERE id=1")
	// Independent ordinary mutations retain compatible shared barrier locks.
	exec(other, "UPDATE memories SET value=value+1 WHERE id=2")
	acquired := make(chan error, 1)
	go func() { _, e := other.Exec(ctx, "SELECT memory_send_guard_begin($1,5000)", token); acquired <- e }()
	select {
	case e := <-acquired:
		t.Fatalf("acquired while mutation uncommitted: %v", e)
	case <-time.After(100 * time.Millisecond):
	}
	exec(admin, "COMMIT")
	if e := <-acquired; e != nil {
		t.Fatal(e)
	}
	// Owner-maintained links and audience tags also participate in source
	// eligibility. Their TRUNCATE path must not bypass row-level revision hooks.
	exec(admin, "RESET ROLE")
	for _, table := range names {
		if _, e := admin.Exec(ctx, "TRUNCATE "+table); e == nil || !strings.Contains(e.Error(), "55P03") {
			t.Fatalf("guarded truncate %s: %v", table, e)
		}
	}
	exec(admin, "SET ROLE "+qrole)
	var value int
	if e := admin.QueryRow(ctx, "SELECT value FROM memories WHERE id=1").Scan(&value); e != nil || value != 1 {
		t.Fatal("new source state unavailable", value, e)
	}
	exec(other, "CREATE TEMP TABLE memory_send_barrier(id integer,blocked_until timestamptz); INSERT INTO memory_send_barrier VALUES(1,'-infinity')")
	denied("UPDATE memories SET value=value+1 WHERE id=1")
	exec(other, "DROP TABLE pg_temp.memory_send_barrier")

	denied("UPDATE memories SET value=value+1 WHERE id=1")
	exec(other, "UPDATE memories SET use_count=use_count+1 WHERE id=1")
	exec(admin, "SELECT memory_send_guard_begin($1,5000)", second)
	exec(other, "SELECT memory_send_guard_end($1)", token)
	denied("UPDATE memories SET value=value+1 WHERE id=1")
	exec(other, "SELECT memory_send_guard_end($1)", strings.Repeat("c", 32))
	denied("UPDATE memories SET value=value+1 WHERE id=1")
	exec(other, "SELECT memory_send_guard_end($1)", second)
	exec(other, "UPDATE memories SET value=value+1 WHERE id=1")
	exec(admin, "BEGIN; SELECT memory_send_guard_begin('"+token+"',5000); ROLLBACK")
	exec(other, "UPDATE memories SET value=value+1 WHERE id=1")
	// An old repeatable-read snapshot cannot ignore a later committed lease.
	exec(admin, "BEGIN ISOLATION LEVEL REPEATABLE READ")
	exec(admin, "SELECT * FROM memories")
	exec(other, "SELECT memory_send_guard_begin($1,5000)", token)
	if _, e := admin.Exec(ctx, "UPDATE memories SET value=value+1 WHERE id=1"); e == nil || !strings.Contains(e.Error(), "40001") {
		t.Fatal("old snapshot did not refuse", e)
	}
	exec(admin, "ROLLBACK")
	// Losing the acquiring connection does not release the durable lease.
	if e := other.Close(ctx); e != nil {
		t.Fatal(e)
	}
	if _, e := admin.Exec(ctx, "UPDATE memories SET value=value+1 WHERE id=1"); e == nil {
		t.Fatal("disconnect released lease")
	}
	// A deadline cannot prove that a paused sender will not resume. Protection
	// survives expiry; explicit completion (or verified terminated sender
	// recovery) is required before a mutation can proceed.
	exec(admin, "SELECT pg_sleep(5.1)")
	if _, e := admin.Exec(ctx, "UPDATE memories SET value=value+1 WHERE id=1"); e == nil {
		t.Fatal("deadline released an unresolved send")
	}
	exec(admin, "SELECT memory_send_guard_end($1)", token)
	exec(admin, "UPDATE memories SET value=value+1 WHERE id=1")
}

func TestSourceReleaseSendGuardAcknowledgement(t *testing.T) {
	for _, scenario := range []string{"acquired", "missing-guard", "wrong-duration", "legacy-expiring-guard", "ineligible"} {
		t.Run(scenario, func(t *testing.T) {
			state := &sourceReleaseState{}
			args := sourceReleaseArgs(map[string]any{"request_id": "guarded", "principal": "owner", "project": "app"})
			ref := releaseTestRef()
			ticket, err := state.prepare(args, map[string]any{"facts_projection": map[string]any{"retained_items": []typedProjectionRef{ref}}})
			if err != nil {
				t.Fatal(err)
			}
			args["source_release_ticket"], _ = json.Marshal(ticket)
			args["operation"] = json.RawMessage(`"source-release-plan"`)
			args["send_guard"] = json.RawMessage(`true`)
			plan := sourceReleaseCall(t, state, args)
			acquire := plan["request"].(map[string]any)["revalidation"].(map[string]any)
			release := plan["release_request"].(map[string]any)["revalidation"].(map[string]any)
			if acquire["send_guard"] != "acquire" || release["send_guard"] != "release" || acquire["check_id"] != release["check_id"] {
				t.Fatal(plan)
			}
			reply := map[string]any{"status": "ok", "eligible": true, "check_id": acquire["check_id"], "sources_digest": releaseDigest([]typedProjectionRef{ref}), "send_guard": "acquired", "lease_ms": 5000, "guard_schema_version": 2}
			switch scenario {
			case "missing-guard":
				delete(reply, "send_guard")
			case "legacy-expiring-guard":
				delete(reply, "guard_schema_version")
			case "wrong-duration":
				reply["lease_ms"] = 4000
			case "ineligible":
				reply["eligible"] = false
			}
			args["operation"] = json.RawMessage(`"source-release-result"`)
			args["owner_response"], _ = json.Marshal(reply)
			result := sourceReleaseCall(t, state, args)
			if (result["admitted"] == true) != (scenario == "acquired") {
				t.Fatal(result)
			}
		})
	}
}

func TestSourceSendGuardRequiresHostAuthority(t *testing.T) {
	for _, row := range []struct {
		caller *bus.CommandContext
		want   bool
	}{
		{nil, false}, {&bus.CommandContext{Principal: "caller"}, false},
		{&bus.CommandContext{Authenticated: true, Principal: "caller"}, false},
		{&bus.CommandContext{Authenticated: true, Principal: "owner", UserAuthority: true}, true},
		{&bus.CommandContext{Authenticated: true, Principal: "host", ScopeKind: "service", ScopeID: "server"}, true},
		{&bus.CommandContext{Authenticated: true, Principal: "host", ScopeKind: "service"}, false},
		{&bus.CommandContext{Authenticated: true, Principal: "user", UserAuthority: true, ScopeKind: "project", ScopeID: "app"}, false},
		{&bus.CommandContext{Authenticated: true, Principal: "user", UserAuthority: true, ScopeKind: "workspace", ScopeID: "team"}, false},
	} {
		if sourceSendGuardAllowed(row.caller) != row.want {
			t.Fatal(row)
		}
		if row.want {
			continue
		}
		for _, mode := range []string{"acquire", "release"} {
			args := sourceReleaseArgs(map[string]any{"principal": "forged-owner", "scope_kind": "service", "scope_id": "server", "scope_context": true, "project": "app", "revalidation": sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), SendGuard: mode, Sources: []typedProjectionRef{releaseTestRef()}}})
			encoded, status := handleSourceRevalidation(handlerOptions{placement: PlacementKB, commandContext: row.caller}, bus.ModuleInvocation{}, "revalidate_sources", args)
			body, err := bus.DecodeCommandResult(encoded)
			if status != bus.ModuleStatusOK || err != nil || !strings.Contains(string(body), `"unauthorized"`) {
				t.Fatal(status, err, string(body))
			}
		}
	}
}

func TestSourceSendCompletionNeedsOnlyOpaqueToken(t *testing.T) {
	completion := sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("a", 32), SendGuard: "release"}
	if !completion.valid() {
		t.Fatal("completion required retained source payload")
	}
	completion.SendGuard = "acquire"
	if completion.valid() {
		t.Fatal("source-free acquisition admitted")
	}
	completion.SendGuard = ""
	if completion.valid() {
		t.Fatal("source-free inspection admitted")
	}
	completion.SendGuard = "release"
	completion.CheckID = "invalid"
	if completion.valid() {
		t.Fatal("invalid completion token admitted")
	}
}

func TestHardRuleSourceObservationsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(q string) {
		t.Helper()
		if _, e := tx.Exec(ctx, q); e != nil {
			t.Fatal(e)
		}
	}
	// Match handleData: bounded source checks disable JIT before planning.
	exec(`SET LOCAL jit=off; DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
 GRANT SELECT ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
 DELETE FROM rules;
 GRANT SELECT ON rules,memory_collection_owner TO aimee_store_runtime;
 GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO aimee_store_runtime;
 SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	observe := func() *sourceRevalidation {
		t.Helper()
		rules, collection, e := backend.recallHardRules(ctx, 32768)
		if e != nil || collection == nil {
			t.Fatal(e)
		}
		p, _, e := projectNativeRecall(recallBundle{AlwaysOnRules: rules, RuleCollection: collection}, 32768)
		if e != nil {
			t.Fatal(e)
		}
		r := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("d", 32), Sources: p.Sources}
		if !r.valid() {
			t.Fatal("invalid rule sources", p)
		}
		return r
	}
	check := func(r *sourceRevalidation, want bool) {
		t.Helper()
		got, e := backend.revalidateSources(ctx, r, Scope{})
		if e != nil || got != want {
			t.Fatal("rule revalidation", got, want, e)
		}
	}
	empty := observe()
	if len(empty.Sources) != 1 {
		t.Fatal("missing empty collection observation")
	}
	check(empty, true)
	exec(`RESET ROLE; INSERT INTO rules(polarity,title,description,directive_type,created_at,updated_at) VALUES('must','required','preserve this constraint','hard',now()::text,now()::text); SET LOCAL ROLE aimee_store_runtime`)
	check(empty, false)
	selected := observe()
	if len(selected.Sources) != 2 {
		t.Fatal("missing selected rule", selected)
	}
	check(selected, true)
	for _, q := range []string{
		`UPDATE rules SET description='changed'`,
		`UPDATE rules SET description='changed';UPDATE rules SET description='preserve this constraint'`,
		`UPDATE rules SET directive_type='soft'`,
		`UPDATE rules SET expires_at=now()::text`,
		`DELETE FROM rules`,
		`INSERT INTO rules(polarity,title,directive_type,created_at,updated_at) VALUES('must','additional','hard',now()::text,now()::text)`,
	} {
		exec("SAVEPOINT rule_edit;RESET ROLE")
		exec(q)
		exec("SET LOCAL ROLE aimee_store_runtime")
		check(selected, false)
		exec("ROLLBACK TO SAVEPOINT rule_edit;RELEASE SAVEPOINT rule_edit")
		check(selected, true)
	}
	selected.SendGuard = "acquire"
	if ok, e := backend.guardedSourceRevalidation(ctx, selected, Scope{}); e != nil || !ok {
		t.Fatal("rule guard", ok, e)
	}
	exec("SAVEPOINT refused_rule;RESET ROLE")
	if _, e := tx.Exec(ctx, `UPDATE rules SET description='raced'`); e == nil || !strings.Contains(e.Error(), "55P03") {
		t.Fatal("guarded rule admitted", e)
	}
	exec("ROLLBACK TO SAVEPOINT refused_rule;RELEASE SAVEPOINT refused_rule;SET LOCAL ROLE aimee_store_runtime")
	selected.SendGuard = "release"
	selected.Sources = nil
	if ok, e := backend.guardedSourceRevalidation(ctx, selected, Scope{}); e != nil || !ok {
		t.Fatal("rule completion", ok, e)
	}
}

func TestHistoricalMemoryEvidencePostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;GRANT SELECT ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true)`)
	const key = "historical-evidence-mr01"
	var parent, edge int64
	if err = tx.QueryRow(ctx, `INSERT INTO memories(key,content,scope_type,scope_value,lifecycle_state,valid_from,valid_until)
 VALUES($1,'retained old version','project',$1,'superseded','2026-01-01T00:00:00Z','2026-03-01T00:00:00Z') RETURNING id`, key).Scan(&parent); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES($1,'assert','test','system',100,'open')`, key)
	if err = tx.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,valid_from,valid_until,asserted_at,commit_id)
 VALUES($1,'uses','old target','semantic','world_fact','persistent','A',.9,80,'2026-01-01T00:00:00Z','2026-03-01T00:00:00Z','2026-01-01T00:00:00Z',$1) RETURNING id`, key).Scan(&edge); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) VALUES($1,'memory','memory:'||$2::bigint::text,'supports')`, edge, parent)
	exec(`INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,valid_at,invalid_at) VALUES($1,$2,'uses','old target','2026-01-01T00:00:00Z','2026-03-01T00:00:00Z')`, parent, key)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true)`, key)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	at := "2026-02-01T00:00:00Z"
	request := DataRequest{Assertions: &assertionSearchRequest{ValidAt: at}, TypedContext: &typedContextOptions{}}
	hits, e := backend.assertionCandidates(ctx, request, Scope{}, key, 10, "")
	if e != nil || len(hits) != 1 {
		t.Fatalf("authorized historical assertion: %d %v", len(hits), e)
	}
	source := hits[0].sourceVersion()
	source.ReadPolicy = &sourceReadPolicy{ValidAt: at}
	retained := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("f", 32), Sources: []typedProjectionRef{{Channel: "historical_assertions", ID: fmt.Sprint(edge), Source: source}}}
	check := func(want bool) {
		t.Helper()
		ok, e := backend.revalidateSources(ctx, retained, Scope{})
		if e != nil || ok != want {
			t.Fatal("historical release", ok, want, e)
		}
		rows, e := backend.assertionCandidates(ctx, request, Scope{}, key, 10, "")
		if e != nil || (len(rows) == 1) != want {
			t.Fatal("historical candidate exclusion", len(rows), want, e)
		}
		rels, e := backend.RelationSearch(ctx, key, at, 10)
		if e != nil || (len(rels) == 1) != want {
			t.Fatal("historical relation", len(rels), want, e)
		}
	}
	check(true)
	current := DataRequest{Assertions: &assertionSearchRequest{}}
	if rows, e := backend.assertionCandidates(ctx, current, Scope{}, key, 10, ""); e != nil || len(rows) != 0 {
		t.Fatal("historical assertion reentered current recall", rows, e)
	}
	for _, change := range []string{"lifecycle_state='deleted'", "lifecycle_state='revoked'", "lifecycle_state='quarantined'", "scope_value='foreign'", "valid_until='2026-02-01T00:00:00Z'"} {
		exec("SAVEPOINT history_exclusion;RESET ROLE")
		exec("UPDATE memories SET "+change+" WHERE id=$1", parent)
		exec("SET LOCAL ROLE aimee_store_runtime")
		check(false)
		exec("ROLLBACK TO SAVEPOINT history_exclusion;RELEASE SAVEPOINT history_exclusion")
	}
}

func TestAuxiliaryTypedSourceObservationsPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, q, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;GRANT SELECT ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
 GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO aimee_store_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true),set_config('jit','off',true)`)
	const key = "typed-auxiliary-source"
	var parent, relation, signal, procedure int64
	insert := func(q string, id *int64, args ...any) {
		t.Helper()
		if e := tx.QueryRow(ctx, q, args...).Scan(id); e != nil {
			t.Fatal(e)
		}
	}
	insert(`INSERT INTO memories(key,content,scope_type,scope_value) VALUES($1,'source','project',$1) RETURNING id`, &parent, key)
	insert(`INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text) VALUES($1,$2,'uses','target','original summary') RETURNING id`, &relation, parent, key)
	insert(`INSERT INTO learning_signals(signal_type) VALUES('typed-source-fixture') RETURNING id`, &signal)
	insert(`INSERT INTO learning_proposals(signal_id,sink,state,target_key,action_json) VALUES($1,'artifact','committed',$2,jsonb_build_object('scope_kind','project','scope_id',$2::text,'step','original procedure')::text) RETURNING id`, &procedure, signal, key)
	exec(`INSERT INTO learning_observations(observation_id,scope_kind,scope_id,observation_type,summary,status,synthesis_policy_version) VALUES($1,'project',$1,'recurring_failure','original observation','active','fixture')`, key)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true)`, key)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	refs := []typedProjectionRef{}
	appendItem := func(channel string, item typedItem) {
		t.Helper()
		ref := typedProjectionRef{Channel: channel, ID: item.id, Source: item.source}
		raw, _ := json.Marshal(item.value)
		if ref.Source == nil || !validTypedSourceItem(ref, raw) {
			t.Fatal("auxiliary source missing", ref)
		}
		refs = append(refs, ref)
	}
	observations, e := backend.typedObservations(ctx, DataRequest{Project: key}, Scope{})
	if e != nil {
		t.Fatal(e)
	}
	for _, item := range observations {
		if item.id == key {
			appendItem("observations", item)
		}
	}
	procedures, e := backend.typedProcedures(ctx, DataRequest{Project: key}, Scope{})
	if e != nil {
		t.Fatal(e)
	}
	for _, item := range procedures {
		if item.id == fmt.Sprint(procedure) {
			appendItem("approved_procedures", item)
		}
	}
	summary, e := backend.typedRelationSummary(ctx, key, Scope{})
	if e != nil || summary == nil {
		t.Fatal("summary observation", summary, e)
	}
	appendItem("summaries", *summary)
	if len(refs) != 3 {
		t.Fatal("incomplete auxiliary sources", refs)
	}
	request := &sourceRevalidation{SchemaVersion: 1, CheckID: strings.Repeat("9", 32), Sources: refs}
	check := func(want bool) {
		t.Helper()
		ok, e := backend.revalidateSources(ctx, request, Scope{})
		if e != nil || ok != want {
			t.Fatal("auxiliary revalidation", ok, want, e)
		}
	}
	check(true)
	for _, q := range []string{
		`UPDATE learning_observations SET summary='changed' WHERE observation_id='` + key + `'`,
		`UPDATE learning_observations SET status='retired' WHERE observation_id='` + key + `'`,
		`UPDATE learning_observations SET scope_id='foreign' WHERE observation_id='` + key + `'`,
		fmt.Sprintf(`UPDATE learning_proposals SET state='archived' WHERE id=%d`, procedure),
		fmt.Sprintf(`UPDATE learning_proposals SET action_json=jsonb_set(action_json::jsonb,'{step}','"changed"')::text WHERE id=%d`, procedure),
		fmt.Sprintf(`UPDATE memory_relations SET fact_text='changed' WHERE id=%d`, relation),
		fmt.Sprintf(`UPDATE memories SET content='changed' WHERE id=%d`, parent),
		fmt.Sprintf(`UPDATE memories SET lifecycle_state='revoked' WHERE id=%d`, parent),
		fmt.Sprintf(`UPDATE memories SET scope_value='foreign' WHERE id=%d`, parent),
	} {
		exec("SAVEPOINT auxiliary_change;RESET ROLE")
		exec(q)
		exec("SET LOCAL ROLE aimee_store_runtime")
		check(false)
		exec("ROLLBACK TO SAVEPOINT auxiliary_change;RELEASE SAVEPOINT auxiliary_change")
		check(true)
	}
	request.SendGuard = "acquire"
	if ok, e := backend.guardedSourceRevalidation(ctx, request, Scope{}); e != nil || !ok {
		t.Fatal("auxiliary guard", ok, e)
	}
	for _, q := range []string{`UPDATE learning_observations SET summary='raced' WHERE observation_id='` + key + `'`, fmt.Sprintf(`UPDATE learning_proposals SET state='archived' WHERE id=%d`, procedure), fmt.Sprintf(`UPDATE memory_relations SET fact_text='raced' WHERE id=%d`, relation)} {
		exec("SAVEPOINT refused_auxiliary;RESET ROLE")
		if _, e := tx.Exec(ctx, q); e == nil || !strings.Contains(e.Error(), "55P03") {
			t.Fatal("unguarded auxiliary mutation", e)
		}
		exec("ROLLBACK TO SAVEPOINT refused_auxiliary;RELEASE SAVEPOINT refused_auxiliary;SET LOCAL ROLE aimee_store_runtime")
	}
	request.SendGuard = "release"
	request.Sources = nil
	if ok, e := backend.guardedSourceRevalidation(ctx, request, Scope{}); e != nil || !ok {
		t.Fatal("auxiliary completion", ok, e)
	}
}
