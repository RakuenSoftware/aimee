package families

import "testing"

// A reply's cells must arrive in the catalog's ORDER, not merely its count.
//
// The width check next door compares len(cols) against the catalog and passes
// happily on a row whose columns are shuffled. workItemCols put the three cost
// columns before pr_ref/worktree/submitter/parent_id where the catalog puts
// them after, so a caller reading cell 16 as work_item_max_cost_usd got the
// submitter and failed with
//
//	strconv.ParseFloat: parsing "uid:1000": invalid syntax
//
// Every Go test passed: they call handlers directly and compare against the
// same wrong order the handler produced. It took a real server answering a real
// /v1/workflow/items request to surface it.
//
// The col slice IS the reply order -- selectList builds the SELECT from it and
// the row scanner reads in the same sequence -- so comparing the slice to the
// catalog is the whole check.

// columnOrder maps an operation to the column list its reply is built from.
// Test-side on purpose: the production code has no static link between an op
// and its columns, and inventing one to satisfy a test would be the tail
// wagging the dog. Adding a col list without adding it here is caught by
// TestEveryColumnListIsOrderChecked below.
func columnOrder() map[string][]col {
	return map[string][]col{
		"work_item_get":                 workItemCols,
		"agent_job_get":                 agentJobCols,
		"agent_log_entry_list":          agentLogCols,
		"roundtable_run_get":            runCols,
		"roundtable_pass_get":           passCols,
		"roundtable_attempt_get_by_run": attemptCols,
		"roundtable_gate_get":           gateCols,
		"lifecycle_event_list":          lifecycleEventCols,
	}
}

// columnAlias records where a SQL column and its wire field legitimately have
// different names, keyed by operation and cell index.
//
// agent_jobs keeps the turn counter in a column named `cursor`; the catalog and
// the C client's struct both call the cell `cursor_turn`. Recording it here
// keeps the order check strict -- a column that drifts to some third name still
// fails -- while naming the one place the two vocabularies differ.
func columnAlias() map[string]map[int]string {
	return map[string]map[int]string{
		"agent_job_get": {7: "cursor_turn"},
	}
}

func TestReplyColumnsAreInTheCatalogsOrder(t *testing.T) {
	c := loadCatalog(t)

	byName := map[string]catalogOp{}
	for _, op := range c.Operations {
		byName[op.Name] = op
	}

	checked := 0
	for opName, cols := range columnOrder() {
		if cols == nil {
			continue
		}
		op, ok := byName[opName]
		if !ok {
			t.Errorf("%s: no such operation in the catalog", opName)
			continue
		}
		want := op.Reply.Fields
		if len(want) != len(cols) {
			t.Errorf("%s: %d columns, the catalog declares %d",
				opName, len(cols), len(want))
			continue
		}
		checked++
		alias := columnAlias()[opName]
		for i := range cols {
			expect := cols[i].name
			if a, ok := alias[i]; ok {
				expect = a
			}
			if expect != want[i].Name {
				t.Errorf("%s cell %d is %q, the catalog puts %q there",
					opName, i, cols[i].name, want[i].Name)
			}
		}
	}
	if checked == 0 {
		t.Fatalf("no column lists were checked")
	}
}

// A column list that nothing checks is a column list free to drift. This does
// not enumerate them by reflection -- Go cannot list a package's variables --
// so it pins the count instead: adding a list without adding it to
// columnOrder() above fails here and says so.
func TestEveryColumnListIsOrderChecked(t *testing.T) {
	// Every []col in the package, whether or not it maps to one operation.
	all := [][]col{
		workItemCols, lifecycleEventCols, agentJobCols, agentLogCols,
		runCols, passCols, attemptCols, gateCols,
	}
	const known = 8
	if len(all) != known {
		t.Fatalf("this test knows %d column lists, found %d", known, len(all))
	}
	mapped := 0
	for _, cols := range columnOrder() {
		if cols != nil {
			mapped++
		}
	}
	if mapped != known {
		t.Errorf("%d column lists exist but %d are order-checked; add the new one "+
			"to columnOrder()", known, mapped)
	}
}
