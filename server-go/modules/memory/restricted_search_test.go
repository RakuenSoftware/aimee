package memory

import (
	"context"
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
)

// Integration uses temporary tables on one connection, never authoritative data.
type restrictedDB struct{ conn *pgx.Conn }

func (d restrictedDB) Query(ctx context.Context, q string, args ...any) (store.Rows, error) {
	return d.conn.Query(ctx, q, args...)
}
func (d restrictedDB) QueryRow(ctx context.Context, q string, args ...any) store.Row {
	return d.conn.QueryRow(ctx, q, args...)
}
func (d restrictedDB) Exec(ctx context.Context, q string, args ...any) (store.Tag, error) {
	return d.conn.Exec(ctx, q, args...)
}
func TestRestrictedSearchDatabase(t *testing.T) {
	dsn := os.Getenv("AIMEE_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set disposable database URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal("test database connection failed")
	}
	defer conn.Close(ctx)
	if _, err = conn.Exec(ctx, "BEGIN"); err != nil {
		t.Fatal(err)
	}
	defer conn.Exec(ctx, "ROLLBACK")
	schema := fmt.Sprintf("restricted_test_%d", time.Now().UnixNano())
	if _, err = conn.Exec(ctx, "CREATE SCHEMA "+schema+"; SET LOCAL search_path="+schema+",pg_temp,public"); err != nil {
		t.Fatal(err)
	}
	_, err = conn.Exec(ctx, `CREATE TEMP TABLE user_memories(id bigint,tier text,kind text,key text,content text,confidence double precision,lifecycle_state text,valid_until timestamptz,updated_at timestamptz);
 INSERT INTO user_memories VALUES
 (5,'L3','fact','drink','unit-alpha prefers oolong',0.95,'active',NULL,now()),
 (6,'L3','fact','city','unit-beta lives in Lima',0.94,'active',NULL,now()),
 (7,'L3','fact','hidden','unit-alpha unit-beta unit-alpha unit-beta forbidden',1,'active',NULL,now()),
 (8,'L3','fact','retired','unit-alpha oolong',1,'retired',NULL,now()),
 (9,'L3','fact','expired','unit-alpha oolong',1,'active',now()-interval '1 day',now());`)
	if err != nil {
		t.Fatal(err)
	}
	backend := postgresDataStore{db: restrictedDB{conn}, placement: PlacementServer}
	for _, tc := range []struct {
		name, query string
		ids, want   []int64
		limit       int
	}{
		{"filter before top one", "unit-alpha unit-beta", []int64{5, 6}, []int64{5}, 1},
		{"composition", "Authoritative user unit-alpha unit-beta assistant think", []int64{5, 6}, []int64{5, 6}, 2},
		{"singleton", "unit-alpha unit-beta", []int64{6}, []int64{6}, 2},
		{"empty grant", "unit-alpha", []int64{}, []int64{}, 2},
		{"lifecycle", "unit-alpha", []int64{8, 9}, []int64{}, 2},
		{"measurement units are not an entity", "CODATA numerical value units ratio", []int64{5, 6}, []int64{}, 2},
		{"entity fragments are not addresses", "unit alpha beta", []int64{5, 6}, []int64{}, 2},
		{"generic single overlap in noisy request", "capital city country population", []int64{5, 6}, []int64{}, 2},
		{"single-term precise request", "Lima", []int64{5, 6}, []int64{6}, 2},
		{"exact compound entity", "unit-beta", []int64{5, 6}, []int64{6}, 2},
		{"ordinary semantic words still match", "prefers oolong", []int64{5, 6}, []int64{5}, 2},
		{"missing", "zxq-no-match", []int64{5, 6}, []int64{}, 2},
		{"operator text", "' OR 1=1 --", []int64{5, 6}, []int64{}, 2},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := SearchRestriction{Version: 1, Namespace: "server", Authorization: strings.Repeat("a", 64), IDs: tc.ids}
			records, e := backend.SearchRestricted(ctx, Scope{ScopeUser, "_user"}, tc.query, "", "", tc.limit, r)
			if e != nil {
				t.Fatal(e)
			}
			ids := []int64{}
			for _, r := range records {
				ids = append(ids, r.ID)
			}
			if !reflect.DeepEqual(ids, tc.want) {
				t.Fatalf("got %v want %v", ids, tc.want)
			}
		})
	}
	// Explicitly exercise KB scope, activation and temporal gates as well.
	_, err = conn.Exec(ctx, `CREATE TEMP TABLE memories (LIKE user_memories INCLUDING ALL);
 -- Mirror the KB schema timestamp helper in a transaction-local scratch schema (rolled back).
 CREATE FUNCTION aimee_utc_text_timestamptz(value text) RETURNS timestamptz AS $$
 SELECT CASE WHEN value IS NULL OR btrim(value)='' THEN NULL
 WHEN value ~ '[T ][0-9]{2}:[0-9]{2}(:[0-9]{2})?([.][0-9]+)?(Z|[+-][0-9]{2}(:?[0-9]{2})?)$' THEN value::timestamptz
 ELSE value::timestamp AT TIME ZONE 'UTC' END
 $$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE SET search_path=pg_catalog;

 ALTER TABLE memories ALTER valid_until TYPE text;
 ALTER TABLE memories ADD scope_type text, ADD scope_value text, ADD activation_suppressed int DEFAULT 0, ADD valid_from text;
 INSERT INTO memories SELECT *, 'project','alpha',0,NULL FROM user_memories;
 UPDATE memories SET scope_value='beta' WHERE id=6;
 UPDATE memories SET activation_suppressed=1 WHERE id=7;
 INSERT INTO memories(id,tier,kind,key,content,confidence,lifecycle_state,updated_at,scope_type,scope_value,activation_suppressed,valid_from) VALUES (10,'L3','fact','future','unit-alpha',1,'active',now(),'project','alpha',0,'2999-01-01');`)
	if err != nil {
		t.Fatal(err)
	}
	backend.placement = PlacementKB
	r := SearchRestriction{Version: 1, Namespace: "kb", Authorization: strings.Repeat("a", 64), IDs: []int64{5, 6, 7, 8, 9, 10}}
	records, err := backend.SearchRestricted(ctx, Scope{ScopeProject, "alpha"}, "unit-alpha unit-beta", "", "", 10, r)
	if err != nil {
		t.Fatal(err)
	}
	if len(records) != 1 || records[0].ID != 5 {
		t.Fatalf("scope/lifecycle leakage: %+v", records)
	}
}
func TestRestrictionNotSilentlyIgnored(t *testing.T) {
	for _, raw := range []string{`{"operation":"search","restriction":{"version":1}}`, `{"operation":"list","restriction":{"version":1}}`} {
		if _, err := decodeDataRequest([]byte(raw)); err == nil {
			t.Fatal("restriction silently ignored")
		}
	}
}
