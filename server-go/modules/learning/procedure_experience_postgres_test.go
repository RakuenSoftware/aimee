package learning

import (
	"context"
	"encoding/json"
	"github.com/jackc/pgx/v5"
	"os"
	"strings"
	"testing"
)

func TestProcedureLedgerPostgresImmutableScopedVersionsAndErasure(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("PostgreSQL fixture unavailable")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal("fixture connection unavailable")
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, e := tx.Exec(ctx, sql, args...); e != nil {
			t.Fatal(e)
		}
	}
	exec(`SELECT set_config('aimee.principal','alice',true)`)
	event, _, _ := procedureFixture()
	raw, _ := json.Marshal(event)
	projection, _ := ProjectProcedureExperience([]ProcedureEvent{event})
	body, _ := json.Marshal(projection)
	admit := func(raw []byte) map[string]any {
		t.Helper()
		var result []byte
		if e := tx.QueryRow(ctx, `SELECT learning_procedure_admit($1::jsonb,$2::jsonb,'project','mr15-test')`, string(raw), string(body)).Scan(&result); e != nil {
			t.Fatal(e)
		}
		var out map[string]any
		json.Unmarshal(result, &out)
		return out
	}
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime`)
	schema, err := os.ReadFile("../../../src/modules/kb/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $procedure_experience_grants$"), strings.Index(string(schema), "END $procedure_experience_grants$;")
	if start < 0 || end < start {
		t.Fatal("production procedure grants unavailable")
	}
	exec(string(schema[start : end+len("END $procedure_experience_grants$;")]))
	// Exercise the same restricted role used by the native learning adapter.
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	first := admit(raw)
	if first["duplicate"] != false || admit(raw)["duplicate"] != true {
		t.Fatal("idempotency lost")
	}
	var visible int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM learning_procedure_events($1::jsonb)`, `{"owner_id":"kb","procedure_id":"procedure:1","revision":"3"}`).Scan(&visible); err != nil || visible != 1 {
		t.Fatal("runtime cohort read unavailable", err, visible)
	}
	exec(`RESET ROLE`)
	var n int
	tx.QueryRow(ctx, `SELECT count(*) FROM learning_application_events WHERE application_id=$1`, first["event_ref"]).Scan(&n)
	if n != 1 {
		t.Fatal(n)
	}
	reject := func(sql string, args ...any) {
		t.Helper()
		exec("SAVEPOINT rejected")
		if _, e := tx.Exec(ctx, sql, args...); e == nil {
			t.Fatal("accepted invalid mutation")
		}
		exec("ROLLBACK TO SAVEPOINT rejected; RELEASE SAVEPOINT rejected")
	}
	event.State = "verified_failure"
	changed, _ := json.Marshal(event)
	reject(`SELECT learning_procedure_admit($1::jsonb,$2::jsonb,'project','mr15-test')`, string(changed), string(body))
	reject(`UPDATE learning_application_events SET outcome='success' WHERE application_id=$1`, first["event_ref"])
	reject(`UPDATE learning_application_events SET governed_event='{}'::jsonb WHERE application_id=$1`, first["event_ref"])
	exec(`SELECT set_config('aimee.principal','bob',true)`)
	reject(`SELECT learning_procedure_admit($1::jsonb,$2::jsonb,'project','mr15-test')`, string(raw), string(body))
	var absent bool
	if e := tx.QueryRow(ctx, `SELECT learning_procedure_experience('kb','procedure:1','3') IS NULL`).Scan(&absent); e != nil || !absent {
		t.Fatal("foreign projection exposed", e)
	}
	exec(`SELECT set_config('aimee.principal','alice',true)`)
	if e := tx.QueryRow(ctx, `SELECT learning_procedure_experience('kb','procedure:1','4') IS NULL`).Scan(&absent); e != nil || !absent {
		t.Fatal("versions blended", e)
	}
	event.State = "verified_success"
	event.ID = "event-2"
	event.Trial = "trial-2"
	raw, _ = json.Marshal(event)
	second := admit(raw)
	exec(`DELETE FROM learning_application_events WHERE application_id=$1`, first["event_ref"])
	if e := tx.QueryRow(ctx, `SELECT experience_projection IS NULL FROM learning_application_events WHERE application_id=$1`, second["event_ref"]).Scan(&absent); e != nil || !absent {
		t.Fatal("erased evidence retained by projection", e)
	}
}
