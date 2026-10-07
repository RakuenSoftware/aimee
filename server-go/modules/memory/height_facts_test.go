package memory

import (
	"context"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
	"strings"
	"testing"
)

func TestHeightFactsKeepDistinctQualifiedNamesAndSourceSpans(t *testing.T) {
	actor := FactActor{Principal: "connector", Role: "user", Rank: 30, Authenticated: 1}
	source := "But Kibukx is 6 feet tall, the Kibukx mountains are 69 feet tall."
	facts := patternFactCandidates(source, "now", 1, 2, actor)
	if len(facts) != 2 || facts[0].Subject != "Kibukx" || facts[0].Object != "6 feet" || facts[1].Subject != "Kibukx mountains" || facts[1].Object != "69 feet" {
		t.Fatalf("%+v", facts)
	}
	if facts[0].Relation != "has_height" || facts[0].Actor != actor || facts[0].Evidence.SourceSpan == facts[1].Evidence.SourceSpan || !factFunctional("has_height") {
		t.Fatalf("%+v", facts)
	}
}

func TestHeightFactsAbstainOnQuestionsNegationAndAmbiguousSpeakers(t *testing.T) {
	for _, text := range []string{"How tall is Kibukx?", "Is Kibukx 6 feet tall?", "Kibukx is not 6 feet tall", "Kibukx is 6 feet tall?", "He is 6 feet tall", "My brother is 6 feet tall", `Someone said Kibukx is 6 feet tall`, `"Kibukx is 6 feet tall"`} {
		if got := heightFactCandidates(text, "now", 1, 2, modelFactActor()); len(got) != 0 {
			t.Fatalf("%q: %+v", text, got)
		}
	}
}

func TestHeightFactsAcceptMeasurementsAndRetainModelAuthority(t *testing.T) {
	for _, text := range []string{"The tower is 6.5 metres tall.", "Fern Peak is 800 meters tall", "Pip is 6 ft tall"} {
		got := heightFactCandidates(text, "now", 1, 2, modelFactActor())
		if len(got) != 1 || got[0].Actor.Rank != 10 {
			t.Fatalf("%q: %+v", text, got)
		}
	}
}

// Called by the rollback-only PostgreSQL fact replay under the real runtime
// role, triggers, evidence guards and WORM commit contract.
func exerciseHeightCaptureReplay(t *testing.T, ctx context.Context, tx pgx.Tx, s *postgresDataStore) {
	t.Helper()
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT height_capture`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT height_capture; RELEASE SAVEPOINT height_capture`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true),set_config('aimee.principal','test:height-caller',true),set_config('aimee.authority','operator',true),set_config('aimee.transport_identity','height-transport',true),set_config('aimee.correlation_id','height-parent',true)`)
	caller := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "test:height-source", TransportIdentity: "height-source-transport"}
	var sourceID int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','height-initial','Kibukx is 6 feet tall, the Kibukx mountains are 69 feet tall.','global','_global') RETURNING id`).Scan(&sourceID); err != nil {
		t.Fatal(err)
	}
	if err := s.captureStoredFactActor(ctx, sourceID, AuthorityUser, caller); err != nil {
		t.Fatal(err)
	}
	var principal, role, transport, correlation string
	if err := tx.QueryRow(ctx, `SELECT current_setting('aimee.principal'),current_setting('aimee.authority'),current_setting('aimee.transport_identity'),current_setting('aimee.correlation_id')`).Scan(&principal, &role, &transport, &correlation); err != nil || principal != "test:height-caller" || role != "operator" || transport != "height-transport" || correlation != "height-parent" {
		t.Fatal("caller context was not restored", principal, role, transport, correlation, err)
	}
	if err := s.captureStoredFactActor(ctx, sourceID, AuthorityUser, caller); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_evidence WHERE source_kind='memory' AND source_id=$1`, fmt.Sprintf("memory:%d", sourceID)).Scan(&count); err != nil || count != 2 {
		t.Fatal("source replay duplicated evidence", count, err)
	}
	var correctionID int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','height-correction','But Kibukx is 8 feet tall.','global','_global') RETURNING id`).Scan(&correctionID); err != nil {
		t.Fatal(err)
	}
	if err := s.captureStoredFactActor(ctx, correctionID, AuthorityUser, caller); err != nil {
		t.Fatal(err)
	}
	// Replaying an older extraction must not resurrect its superseded assertion.
	if err := s.captureStoredFactActor(ctx, sourceID, AuthorityUser, caller); err != nil {
		t.Fatal(err)
	}
	for _, expected := range [][3]string{{"Kibukx", "6 feet", "superseded"}, {"Kibukx", "8 feet", "persistent"}, {"Kibukx mountains", "69 feet", "persistent"}} {
		var state string
		if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM entity_edges WHERE edge_class='semantic' AND source=$1 AND relation='has_height' AND target=$2`, expected[0], expected[1]).Scan(&state); err != nil || state != expected[2] {
			t.Fatal(expected, state, err)
		}
	}
	exec(`SET CONSTRAINTS ALL IMMEDIATE`)
}

func TestHeightFactsBoundLongNotes(t *testing.T) {
	facts := heightFactCandidates(strings.Repeat("Fern Peak is 6 feet tall. ", memoryFactMaxTriples*3), "now", 1, 2, modelFactActor())
	if len(facts) != memoryFactMaxTriples {
		t.Fatal(len(facts))
	}
}
