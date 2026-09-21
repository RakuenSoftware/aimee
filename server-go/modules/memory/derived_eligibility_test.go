package memory

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// Exercise the actual serving queries under the packaged database's runtime
// role. One transaction clock keeps subsecond and offset boundaries exact.
func exerciseDerivedEligibilityReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT derived_eligibility`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT derived_eligibility; RELEASE SAVEPOINT derived_eligibility`) }()
	// Preserve exact locator equality, including boundary IDs, while making the
	// parent primary key usable. Bad locators must neither match nor overflow.
	for _, locator := range []string{"memory:0", "memory:1", "memory:-1", "memory:9223372036854775807", "memory:-9223372036854775808", "memory:9223372036854775808", "memory:-9223372036854775809", "memory:01", "memory:-0", "memory:+1", "memory:1.0", "memory:1e0", "memory: 1", "memory:1\n", "Memory:1", "memory:", "other:1"} {
		var same bool
		if err := tx.QueryRow(ctx, `WITH ids(id) AS (VALUES(0::bigint),(1),(-1),(9223372036854775807),(-9223372036854775808))
 SELECT (SELECT array_agg(id ORDER BY id) FROM ids WHERE $1='memory:'||id::text)
 IS NOT DISTINCT FROM (SELECT array_agg(id ORDER BY id) FROM ids WHERE id=`+memoryLocatorIDSQL("$1::text")+`)`, locator).Scan(&same); err != nil || !same {
			t.Fatalf("canonical locator behavior changed for %q: same=%v err=%v", locator, same, err)
		}
	}
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true); SET LOCAL TIME ZONE 'Asia/Tokyo'`)
	const subject = "DerivedValidityFixture"
	var parent, episode, edge, codeEdge, scene int64
	var now time.Time
	if err := tx.QueryRow(ctx, `SELECT CURRENT_TIMESTAMP`).Scan(&now); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,source_session)
 VALUES('L2','fact',$1,'derived validity source','project',$1,$1) RETURNING id`, subject).Scan(&parent); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session,created_at)
 VALUES($1,$2,'derived validity episode',$2,'9999-01-01T00:00:00Z') RETURNING id`, parent, subject).Scan(&episode); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO memory_relations(memory_id,episode_id,src_entity,relation,dst_entity,fact_text) VALUES($1,$2,$3,'owns','target','derived validity fact')`, parent, episode, subject)
	exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,$2)`, parent, subject)
	exec(`INSERT INTO memory_units(memory_id,unit_type,unit_text,is_episode_card) VALUES($1,'episode','derived validity card',1)`, parent)
	exec(`INSERT INTO memory_summaries(memory_id,scope,summary) VALUES($1,'headline','derived validity summary')`, parent)
	if err := tx.QueryRow(ctx, `INSERT INTO memory_scenes(workspace_id,created_at) VALUES($1,'9999-01-01T00:00:00Z') RETURNING id`, subject).Scan(&scene); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO memory_scene_members(scene_id,memory_id) VALUES($1,$2)`, scene, parent)
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('derived-validity-fixture','assert','test','system',100,'open')`)
	if err := tx.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,commit_id)
 VALUES($1,'naming_convention','fixture','semantic','world_fact','persistent','A',.9,80,'derived-validity-fixture') RETURNING id`, subject).Scan(&edge); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target) VALUES($1,'calls','derived:target') RETURNING id`, subject).Scan(&codeEdge); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) SELECT unnest($1::bigint[]),'memory','memory:'||$2::bigint::text,'supports'`, []int64{edge, codeEdge}, parent)
	exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true)`, subject)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	req := DataRequest{Project: subject, Assertions: &assertionSearchRequest{}, TypedContext: &typedContextOptions{}}
	check := func(want bool) {
		t.Helper()
		n := 0
		if want {
			n = 1
		}
		episodes, err := backend.EpisodeList(ctx, subject, 1)
		if err != nil || len(episodes) != n {
			t.Fatalf("episode list want %d: %+v %v", n, episodes, err)
		}
		_, err = backend.EpisodeGet(ctx, subject)
		if want && err != nil || !want && !errors.Is(err, ErrMemoryNotFound) {
			t.Fatalf("episode get eligible=%v: %v", want, err)
		}
		cards, err := backend.episodeCards(ctx, subject, 1)
		if err != nil || len(cards) != n {
			t.Fatalf("episode cards want %d: %+v %v", n, cards, err)
		}
		summaries, err := backend.Summaries(ctx, parent, 1)
		if err != nil || len(summaries) != n {
			t.Fatalf("summaries want %d: %+v %v", n, summaries, err)
		}
		members, err := backend.SceneMembers(ctx, scene, 1)
		if err != nil || len(members) != n {
			t.Fatalf("scene members want %d: %+v %v", n, members, err)
		}
		scenes, err := backend.Scenes(ctx, 1)
		if err != nil || (len(scenes) == 1 && scenes[0].ID == scene) != want {
			t.Fatalf("scenes eligible=%v: %+v %v", want, scenes, err)
		}
		relations, err := backend.RelationSearch(ctx, subject, "", 1)
		if err != nil || len(relations) != n {
			t.Fatalf("relations want %d: %+v %v", n, relations, err)
		}
		relations, err = backend.EntityEdges(ctx, subject, 1)
		if err != nil || len(relations) != n {
			t.Fatalf("entity edges want %d: %+v %v", n, relations, err)
		}
		profile, err := backend.EntityProfile(ctx, subject)
		if want && (err != nil || profile.Mentions != 1 || profile.Relations != 2) || !want && !errors.Is(err, ErrMemoryNotFound) {
			t.Fatalf("profile eligible=%v: %+v %v", want, profile, err)
		}
		assertions, err := backend.assertionCandidates(ctx, req, Scope{}, subject, 1, "")
		if err != nil || len(assertions) != n {
			t.Fatalf("assertions want %d: %+v %v", n, assertions, err)
		}
		css, err := backend.cssConventions(ctx, subject)
		if err != nil || len(css) != n {
			t.Fatalf("conventions want %d: %+v %v", n, css, err)
		}
		watermark, err := backend.typedWatermarks(ctx, req, Scope{Type: ScopeProject, Value: subject})
		if err != nil || (watermark.Durable == "9999-01-01T00:00:00Z") != want {
			t.Fatalf("watermark eligible=%v: %+v %v", want, watermark, err)
		}
		exec(`UPDATE entity_edges SET utility_score=0 WHERE id=$1`, codeEdge)
		if err := backend.feedbackPath(ctx, DataRequest{Success: true, GraphPath: []GraphPathEntry{{Node: subject, Relation: "calls", Hop: 1}}}); err != nil {
			t.Fatal(err)
		}
		var utility float64
		if err := tx.QueryRow(ctx, `SELECT utility_score FROM entity_edges WHERE id=$1`, codeEdge).Scan(&utility); err != nil || (utility > 0) != want {
			t.Fatalf("feedback eligible=%v: %v %v", want, utility, err)
		}
	}
	for _, tc := range []struct {
		name, from, until, lifecycle string
		suppressed                   int
		want                         bool
	}{
		{"unbounded", "", "", "active", 0, true},
		{"inclusive offset", now.Format(time.RFC3339Nano), "", "active", 0, true},
		{"exclusive offset", "", now.In(time.FixedZone("offset", 9*3600)).Format(time.RFC3339Nano), "active", 0, false},
		{"future fraction", now.Add(time.Microsecond).Format(time.RFC3339Nano), "", "active", 0, false},
		{"unexpired fraction", "", now.Add(time.Microsecond).Format(time.RFC3339Nano), "active", 0, true},
		{"expired", "", now.Add(-time.Second).Format(time.RFC3339Nano), "active", 0, false},
		{"suppressed", "", "", "active", 1, false},
		{"retired", "", "", "retired", 0, false},
	} {
		t.Log("derived serving boundary:", tc.name)
		exec(`UPDATE memories SET valid_from=$1,valid_until=$2,lifecycle_state=$3,activation_suppressed=$4 WHERE id=$5`, tc.from, tc.until, tc.lifecycle, tc.suppressed, parent)
		check(tc.want)
	}
	// A second eligible parent cannot authorize the expired source. This is
	// independent of both RLS and the first parent's continued visibility.
	exec(`UPDATE memories SET lifecycle_state='active',activation_suppressed=0,valid_until=pg_now_text() WHERE id=$1`, parent)
	var second int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','derived-validity-second','valid','project',$1) RETURNING id`, subject).Scan(&second); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) SELECT unnest($1::bigint[]),'memory','memory:'||$2::bigint::text,'supports'`, []int64{edge, codeEdge}, second)
	exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,$2)`, second, subject)
	assertions, err := backend.assertionCandidates(ctx, req, Scope{}, subject, 1, "")
	if err != nil || len(assertions) != 0 {
		t.Fatal("mixed expired evidence admitted", assertions, err)
	}
	css, err := backend.cssConventions(ctx, subject)
	if err != nil || len(css) != 0 {
		t.Fatal("mixed expired conventions admitted", css, err)
	}
	profile, err := backend.EntityProfile(ctx, subject)
	if err != nil || profile.Mentions != 1 || profile.Relations != 0 {
		t.Fatal("mixed expired profile admitted", profile, err)
	}
	exec(`UPDATE entity_edges SET utility_score=0 WHERE id=$1`, codeEdge)
	if err := backend.feedbackPath(ctx, DataRequest{Success: true, GraphPath: []GraphPathEntry{{Node: subject, Hop: 1}}}); err != nil {
		t.Fatal(err)
	}
	var utility float64
	if err := tx.QueryRow(ctx, `SELECT utility_score FROM entity_edges WHERE id=$1`, codeEdge).Scan(&utility); err != nil || utility != 0 {
		t.Fatal("mixed expired feedback reinforced", utility, err)
	}
	// Invalid governed timestamps must fail closed at each adapter, rather than
	// becoming unbounded or being compared lexically. Savepoints recover each
	// deliberately failed query without hiding the expected database error.
	reads := map[string]func() error{
		"episode list":  func() error { _, err := backend.EpisodeList(ctx, subject, 1); return err },
		"episode get":   func() error { _, err := backend.EpisodeGet(ctx, subject); return err },
		"episode cards": func() error { _, err := backend.episodeCards(ctx, subject, 1); return err },
		"summaries":     func() error { _, err := backend.Summaries(ctx, parent, 1); return err },
		"scene members": func() error { _, err := backend.SceneMembers(ctx, scene, 1); return err },
		"relations":     func() error { _, err := backend.RelationSearch(ctx, subject, "", 1); return err },
		"entity edges":  func() error { _, err := backend.EntityEdges(ctx, subject, 1); return err },
		"profile":       func() error { _, err := backend.EntityProfile(ctx, subject); return err },
		"assertions":    func() error { _, err := backend.assertionCandidates(ctx, req, Scope{}, subject, 1, ""); return err },
		"conventions":   func() error { _, err := backend.cssConventions(ctx, subject); return err },
		"watermark": func() error {
			_, err := backend.typedWatermarks(ctx, req, Scope{Type: ScopeProject, Value: subject})
			return err
		},
	}
	for _, bad := range []string{"now", "infinity", "2026-02-30T00:00:00Z"} {
		for name, read := range reads {
			exec(`SAVEPOINT malformed_derived_parent`)
			exec(`UPDATE memories SET valid_from=$1,valid_until='' WHERE id=$2`, bad, parent)
			if err := read(); err == nil {
				t.Fatalf("%s accepted malformed parent %q", name, bad)
			}
			exec(`ROLLBACK TO SAVEPOINT malformed_derived_parent; RELEASE SAVEPOINT malformed_derived_parent`)
		}
	}
}
