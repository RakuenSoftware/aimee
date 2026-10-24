package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strings"
	"testing"
)

// Real PostgreSQL executes the production aggregate, including NULL/no-row behavior.
func TestTokenAuditUsageCostPostgres(t *testing.T) {
	pool := livePool(t)
	ctx := context.Background()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	schema, err := schemaFS.ReadFile("schema_telemetry.sql")
	if err != nil {
		t.Fatal(err)
	}
	table := string(schema[strings.Index(string(schema), "CREATE TABLE IF NOT EXISTS token_audit"):])
	table = strings.SplitN(table, "\n);", 2)[0] + "\n);"
	table = strings.Replace(table, "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE", 1)
	_, err = tx.Exec(ctx, table)
	if err != nil {
		t.Fatal(err)
	}
	q := sessionIRLiveTx{tx}
	check := func(t *testing.T, id string, known bool, value string) {
		t.Helper()
		for _, query := range []string{tokenAuditCostForDelegationSQL, tokenAuditCostForDelegationExSQL} {
			status, fields, err := oneCost(query)(ctx, q, []string{id})
			if known {
				if err != nil || status != store.StatusOK || len(fields) != 1 || fields[0] != value {
					t.Fatalf("%s: %d %q %v", id, status, fields, err)
				}
			} else if status == store.StatusOK {
				t.Fatalf("%s: unobserved cost became known: %q", id, fields)
			}
		}
	}
	t.Run("missing", func(t *testing.T) { check(t, "absent", false, "") })
	insert := func(id, kind, cost string) {
		t.Helper()
		fields := make([]string, 23)
		fields[1], fields[9], fields[22] = id, kind, cost
		for _, i := range []int{10, 13, 16, 18, 19, 20, 21} {
			fields[i] = "0"
		}
		status, _, err := tokenAuditInsert(ctx, q, fields)
		if err != nil || status != store.StatusOK {
			t.Fatalf("actual insert: %d %v", status, err)
		}
	}
	insert("zero", "realized", "0")
	insert("known", "realized", "1.25")
	insert("unknown", "unobserved", "0")
	insert("mixed", "realized", "2")
	insert("mixed", "unobserved", "0")

	t.Run("observed-zero", func(t *testing.T) { check(t, "zero", true, "0") })
	t.Run("known", func(t *testing.T) { check(t, "known", true, "1.25") })
	t.Run("unobserved", func(t *testing.T) { check(t, "unknown", false, "") })
	t.Run("mixed", func(t *testing.T) { check(t, "mixed", false, "") })
}
