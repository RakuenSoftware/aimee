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

// All three production metrics readers preserve uncertainty beside the subtotal.
func TestMetricsUsageKnowledgePostgres(t *testing.T) {
	pool := livePool(t)
	ctx := context.Background()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	for _, item := range []struct{ file, table string }{{"schema_telemetry.sql", "token_audit"}, {"schema_agent_work.sql", "agent_log"}} {
		raw, err := schemaFS.ReadFile(item.file)
		if err != nil {
			t.Fatal(err)
		}
		table := string(raw[strings.Index(string(raw), "CREATE TABLE IF NOT EXISTS "+item.table):])
		table = strings.Replace(strings.SplitN(table, "\n);", 2)[0]+"\n);", "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE", 1)
		if _, err := tx.Exec(ctx, table); err != nil {
			t.Fatal(err)
		}
	}
	// Production is NOT NULL; a legacy/malformed nullable source must remain unknown.
	if _, err := tx.Exec(ctx, "ALTER TABLE token_audit ALTER COLUMN usage_kind DROP NOT NULL"); err != nil {
		t.Fatal(err)
	}
	q := sessionIRLiveTx{tx}
	for _, s := range []struct {
		name          string
		kinds         []string
		costs         []float64
		cost, unknown string
	}{
		{"observed-zero", []string{"realized"}, []float64{0}, "0", "0"},
		{"known", []string{"realized"}, []float64{1.25}, "1.25", "0"},
		{"unknown", []string{"unobserved"}, []float64{0}, "0", "1"},
		{"mixed", []string{"realized", "unobserved"}, []float64{2, 0}, "2", "1"},
		{"missing-audit", []string{"missing"}, []float64{0}, "0", "1"},
		{"estimated-is-not-realized", []string{"estimated"}, []float64{3}, "0", "1"},
		{"avoided-is-not-paid", []string{"avoided"}, []float64{3}, "0", "0"},
		{"legacy-empty", []string{""}, []float64{1}, "1", "0"},
		{"partial", []string{"partial"}, []float64{1}, "0", "1"},
		{"null-kind", []string{"null"}, []float64{0}, "0", "1"},
		{"unrecognized", []string{"future-unknown"}, []float64{4}, "0", "1"},
	} {
		t.Run(s.name, func(t *testing.T) {
			if _, err := tx.Exec(ctx, "TRUNCATE agent_log, token_audit"); err != nil {
				t.Fatal(err)
			}
			for i, kind := range s.kinds {
				var id int64
				if err := tx.QueryRow(ctx, `INSERT INTO agent_log(agent_name,role,success) VALUES('metric-agent','code',true) RETURNING id`).Scan(&id); err != nil {
					t.Fatal(err)
				}
				if kind != "missing" {
					var kindValue any = kind
					if kind == "null" {
						kindValue = nil
					}
					if _, err := tx.Exec(ctx, `INSERT INTO token_audit(agent_log_id,tool_name,role,usage_kind,estimated_cost_usd) VALUES($1,'metric-agent','code',$2,$3)`, id, kindValue, s.costs[i]); err != nil {
						t.Fatal(err)
					}
				}
			}
			for _, fn := range []struct {
				name string
				run  func(context.Context, store.Queryer, []string) (uint32, []string, error)
				args []string
				ci   int
			}{
				{"role", agentLogMetricsByRole, []string{"64"}, 7}, {"agent", agentLogAgentStats, []string{"", "64"}, 8},
			} {
				status, fields, err := fn.run(ctx, q, fn.args)
				if err != nil || status != store.StatusOK || len(fields) != fn.ci+2 || fields[fn.ci] != s.cost || fields[len(fields)-1] != s.unknown {
					t.Fatalf("%s: %d %q %v", fn.name, status, fields, err)
				}
			}
			status, fields, err := tokenAuditListDashboard(ctx, q, []string{"64"})
			if s.name == "missing-audit" || s.name == "avoided-is-not-paid" {
				if err != nil || status != store.StatusOK || len(fields) != 0 {
					t.Fatalf("dashboard ineligible: %d %q %v", status, fields, err)
				}
			} else if err != nil || status != store.StatusOK || len(fields) != 10 || fields[6] != s.cost || fields[9] != s.unknown {
				t.Fatalf("dashboard: %d %q %v", status, fields, err)
			}
		})
	}
}
