package families

import (
	"context"
	"os"
	"reflect"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
)

type traceReplayQueryer struct{ pgx.Tx }

func (q traceReplayQueryer) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	return q.Tx.Exec(ctx, sql, args...)
}
func (q traceReplayQueryer) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	return q.Tx.Query(ctx, sql, args...)
}
func (q traceReplayQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return q.Tx.QueryRow(ctx, sql, args...)
}

func TestTracePaginationPostgres(t *testing.T) {
	url := os.Getenv("AIMEE_DB1_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB1_REPLAY_URL to a disposable PostgreSQL database")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE execution_trace(id bigint,plan_id bigint,turn bigint,direction text,tool_name text,tool_args text,tool_result text);
 INSERT INTO execution_trace VALUES
 (9007199254741508,99,3,'call','Read','{}','error'),
 (9007199254741509,1,1,'call','Search','{}','ok'),
 (9007199254741510,2,2,'call','Read','{}','ok'),
 (9007199254741511,NULL,1,'call','','{}','ok'),
 (9007199254741512,1,2,'call','Edit','{}','ok')`)
	if err != nil {
		t.Fatal(err)
	}
	after := "0"
	var ids []string
	for i := 0; i < 3; i++ {
		status, fields, err := traceListAfterID(ctx, traceReplayQueryer{tx}, []string{after, "2"})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, fields, err)
		}
		for j := 0; j < len(fields); j += 7 {
			ids = append(ids, fields[j])
			after = fields[j]
		}
	}
	want := []string{"9007199254741508", "9007199254741509", "9007199254741510", "9007199254741512"}
	if !reflect.DeepEqual(ids, want) {
		t.Fatal("cursor pagination skipped/repeated traces", ids, want)
	}
}
